// Copyright (c) 2018, Smart Projects Holdings Ltd
// All rights reserved.
// See LICENSE file for license details.

#include <ugcs/vsm/log.h>
#include <ugcs/vsm/debug.h>
#include <ugcs/vsm/cucs_processor.h>
#include <ugcs/vsm/request_context.h>
#include <ugcs/vsm/timer_processor.h>
#include <ugcs/vsm/transport_detector.h>
#include <ugcs/vsm/properties.h>
#include <ugcs/vsm/param_setter.h>

using namespace ugcs::vsm;

constexpr std::chrono::seconds Cucs_processor::WRITE_TIMEOUT;
constexpr std::chrono::seconds Cucs_processor::REGISTER_PEER_TIMEOUT;

constexpr uint32_t Cucs_processor::SUPPORTED_UCS_VERSION_MAJOR;
constexpr uint32_t Cucs_processor::SUPPORTED_UCS_VERSION_MINOR;

Singleton<Cucs_processor> Cucs_processor::singleton;

Cucs_processor::Cucs_processor():
        Request_processor("Cucs processor"),
        ucs_id_counter(1),
        ucs_connector(Transport_detector::Create())
{
}

void
Cucs_processor::Register_device(Device::Ptr vehicle)
{
    auto request = Request::Create();
    auto proc_handler = Make_callback(
            &Cucs_processor::On_register_vehicle,
            Shared_from_this(),
            request,
            vehicle);
    request->Set_processing_handler(proc_handler);
    Submit_request(request);
    // Wait because it accesses Device structures to create registration message.
    request->Wait_done();
}

void
Cucs_processor::Unregister_device(uint32_t handle)
{
    auto request = Request::Create();
    auto proc_handler = Make_callback(
            &Cucs_processor::On_unregister_vehicle,
            Shared_from_this(),
            request,
            handle);
    request->Set_processing_handler(proc_handler);
    Submit_request(request);
}

void
Cucs_processor::Send_ucs_message(uint32_t handle, Proto_msg_ptr message, uint32_t stream_id)
{
    auto request = Request::Create();
    auto proc_handler = Make_callback(
        &Cucs_processor::On_send_ucs_message,
        Shared_from_this(),
        request,
        handle,
        message,
        stream_id);
    request->Set_processing_handler(proc_handler);
    Submit_request(request);
}

void
Cucs_processor::On_enable()
{
    completion_ctx = Request_completion_context::Create("Cucs processor completion");
    worker = Request_worker::Create(
            "Cucs processor worker",
            std::initializer_list<Request_container::Ptr>{completion_ctx, Shared_from_this()});
    completion_ctx->Enable();
    Request_processor::On_enable();
    worker->Enable();

    auto props = Properties::Get_instance();
    if (props->Exists("ucs.disable")) {
        return;
    }

    if (Properties::Get_instance()->Exists("ucs.transport_detector_on_when_diconnected")) {
        transport_detector_on_when_diconnected = true;
        Transport_detector::Get_instance()->Activate(true);
    } else {
        transport_detector_on_when_diconnected = false;
        Transport_detector::Get_instance()->Activate(false);
    }

    if (Properties::Get_instance()->Exists("ucs.keep_alive_timeout")) {
        auto t = Properties::Get_instance()->Get_int("ucs.keep_alive_timeout");
        keep_alive_timeout = std::chrono::seconds(t);
        LOG_INFO("Setting ucs connection_timeout to %d", t);
    }

    timer = Timer_processor::Get_instance()->Create_timer(
        std::chrono::seconds(1),
        Make_callback(
            &Cucs_processor::On_timer,
            Shared_from_this()),
            completion_ctx);

    ucs_connector->Enable();
    ucs_connector->Add_detector(
        ugcs::vsm::Transport_detector::Make_connect_handler(
            &Cucs_processor::On_incoming_connection,
            Shared_from_this()),
        Shared_from_this(),
        "ucs");
}

void
Cucs_processor::On_disable()
{
    timer->Cancel();
    auto req = Request::Create();
    req->Set_processing_handler(
            Make_callback(
                    &Cucs_processor::Process_on_disable,
                    Shared_from_this(),
                    req));
    Submit_request(req);
    req->Wait_done(false);
    Set_disabled();
    ucs_connector->Disable();
    worker->Disable();
}

void
Cucs_processor::Process_on_disable(Request::Ptr request)
{
    if (vehicles.size()) {
        LOG_ERR("%zu vehicles are still present in Cucs processor while disabling.",
                vehicles.size());
        ASSERT(false);
    }

    // TODO clean vehicle shutdown.
    vehicles.clear();

    for (auto& iter : ucs_connections) {
        iter.second.read_waiter.Abort();
        iter.second.stream->Close();
    }
    ucs_connections.clear();

    completion_ctx->Disable();
    completion_ctx = nullptr;

    request->Complete();
}

bool
Cucs_processor::On_timer()
{
    for (auto& iter : ucs_connections) {
        auto now = std::chrono::steady_clock::now();
        if (iter.second.ucs_id) {
            // known ucs.
            if (keep_alive_timeout.count()) {
                // timeout specified.
                if (iter.second.last_message_time + keep_alive_timeout < now) {
                    LOG("Server connection timed out");
                    iter.second.stream->Close();
                } else {
                    // Still good. Send another ping.
                    ugcs::vsm::proto::Vsm_message ping_msg;
                    ping_msg.set_device_id(0);
                    ping_msg.set_response_required(true); // this will set message_id automatically.
                    Send_ucs_message(iter.first, ping_msg);
                }
            }
        } else {
            if (iter.second.last_message_time + REGISTER_PEER_TIMEOUT < now && !iter.second.ucs_id) {
                LOG("Server connection timed out");
                iter.second.stream->Close();
            }
        }
    }
    return true;
}

void
Cucs_processor::On_incoming_connection(std::string, int, Socket_address::Ptr addr, Io_stream::Ref stream)
{
    if (stream->Get_type() != Io_stream::Type::TCP) {
        // support only TCP connections.
        stream->Close();
        return;
    }

    auto new_id = Get_next_id();
    Server_context sc;
    sc.stream = stream;
    sc.stream_id = new_id;
    sc.address = addr;
    sc.last_message_time = std::chrono::steady_clock::now();
    Schedule_next_read(sc);

    ucs_connections.emplace(sc.stream_id, std::move(sc));

    ugcs::vsm::proto::Vsm_message msg;
    auto p = msg.mutable_register_peer();
    p->set_peer_id(Get_application_instance_id());
    p->set_peer_type(proto::PEER_TYPE_VSM);
    // Get the VSM name which must be defined using DEFINE_DEFAULT_VSM_NAME in VSM sources.
    p->set_name(Get_vsm_name());
    p->set_version_major(SDK_VERSION_MAJOR);
    p->set_version_minor(SDK_VERSION_MINOR);
    p->set_version_build(SDK_VERSION_BUILD);
    msg.set_device_id(0);
    Send_ucs_message(new_id, msg);
}

void
Cucs_processor::Schedule_next_read(
        Server_context& sc)
{
    sc.read_waiter.Abort();
    sc.read_waiter = sc.stream->Read(
        sc.to_read,
        sc.to_read,
        Make_read_callback(
            &Cucs_processor::Read_completed,
            Shared_from_this(),
            sc.stream_id),
        completion_ctx);
}

void
Cucs_processor::Read_completed(
        Io_buffer::Ptr buffer,
        Io_result result,
        size_t stream_id)
{
    auto iter = ucs_connections.find(stream_id);
    if (iter == ucs_connections.end()) {
        // stream closed. nothing to do.
        return;
    }
    auto & connection = iter->second;
    if (result == Io_result::OK) {
        if (connection.reading_header) {
            if (buffer->Get_length() != 1) {
                VSM_EXCEPTION(Internal_error_exception, "Internal error");
            }
            int byte = *static_cast<const uint8_t*>(buffer->Get_data());
            connection.message_size |= (byte & 0x7f) << connection.shift;
            if (connection.message_size > PROTO_MAX_MESSAGE_LEN) {
                LOG_ERR("Proto message len of %zu exceeds allowed %zu bytes!",
                    connection.message_size,
                    PROTO_MAX_MESSAGE_LEN);
                Close_ucs_stream(stream_id);
                return;
            }
            if (byte & 0x80) {
                // there is more
                connection.shift += 7;
            } else {
                connection.shift = 0;
                if (connection.message_size) {
                    connection.to_read = connection.message_size;
                    connection.reading_header = false;
                } else {
                    // Zero len message, continue with next header.
                    connection.to_read = 1;
                }
            }
        } else {
            ugcs::vsm::proto::Vsm_message vsm_msg;
            if (vsm_msg.ParseFromArray(buffer->Get_data(), buffer->Get_length())) {
                // Message parsed ok.
                // LOG("received msg: %s", vsm_msg.SerializeAsString().c_str());
                if (connection.ucs_id) {
                    // knwon ucs
                    connection.last_message_time = std::chrono::steady_clock::now();
                    if (vsm_msg.has_device_response()) {
                        // This is a response to VSM request.
                        auto conn_it = connection.pending_registrations.find(vsm_msg.message_id());
                        if (conn_it == connection.pending_registrations.end()) {
                            // This response is not for Register_device. Pass it on.
                            On_ucs_message(stream_id, std::move(vsm_msg));
                        } else {
                            // we have a pending registration.
                            switch (vsm_msg.device_response().code()) {
                            case proto::STATUS_OK: {
                                LOG("Device %d registered with ucs %08X", conn_it->second, *connection.ucs_id);
                                auto it = vehicles.find(conn_it->second);
                                if (it != vehicles.end()) {
                                    connection.registered_devices.insert(conn_it->second);
                                    // Signal device about new connection.
                                    Notify_device_about_ucs_connections(conn_it->second);
                                    // Send cached telemetry data
                                    ugcs::vsm::proto::Vsm_message reg;
                                    reg.set_device_id(it->first);
                                    auto tf = reg.mutable_device_status()->mutable_telemetry_fields();
                                    for (auto& e : it->second.telemetry_cache) {
                                        // Do not send cached telemetry values which are N/A
                                        if (    !e.second.value().has_meta_value()
                                            ||  e.second.value().meta_value() != ugcs::vsm::proto::META_VALUE_NA) {
                                            auto f = tf->Add();
                                            f->CopyFrom(e.second);
                                        }
                                    }
                                    auto av = reg.mutable_device_status()->mutable_command_availability();
                                    for (auto& e : it->second.availability_cache) {
                                        auto f = av->Add();
                                        f->CopyFrom(e.second);
                                    }
                                    Send_ucs_message(connection.stream_id, reg);
                                }
                                connection.pending_registrations.erase(conn_it);
                            } break;
                            case proto::STATUS_IN_PROGRESS:
                                LOG("Device %d registration with ucs %08X in progress (%d%%)",
                                    conn_it->second,
                                    *connection.ucs_id,
                                    static_cast<int>(vsm_msg.device_response().progress() * 100));
                                break;
                            default:
                                LOG("Device %d registration failed with ucs %08X code: %d, reason: %s",
                                    conn_it->second,
                                    *connection.ucs_id,
                                    vsm_msg.device_response().code(),
                                    vsm_msg.device_response().status().c_str());
                                connection.pending_registrations.erase(conn_it);
                            }
                        }
                    } else {
                        // This is not a Device_response message.
                        On_ucs_message(stream_id, std::move(vsm_msg));
                    }
                } else {
                    // ucs id still unknown.
                    if (vsm_msg.has_register_peer()) {
                        // message has Register_peer payload.
                        connection.last_message_time = std::chrono::steady_clock::now();
                        auto& reg_peer = vsm_msg.register_peer();
                        if (!reg_peer.has_peer_type() || reg_peer.peer_type() == proto::PEER_TYPE_SERVER)
                        {
                            // no peer_type assumes server.
                            auto new_peer = reg_peer.peer_id();
                            // Look if it is a duplicate connection
                            auto dupe = false;
                            for (auto& ucs : ucs_connections) {
                                if (ucs.second.ucs_id && *(ucs.second.ucs_id) == new_peer) {
                                    dupe = true;
                                    if (ucs.second.primary) {
                                        if (    !ucs.second.address->Is_loopback_address()
                                            ||  connection.address->Is_loopback_address()) {
                                            ucs.second.primary = false;
                                            connection.primary = true;
                                            LOG("Switched primary connection for %08X from %s to %s",
                                                new_peer,
                                                ucs.second.stream->Get_name().c_str(),
                                                connection.stream->Get_name().c_str());
                                        }
                                        break;
                                    }
                                }
                            }

                            uint32_t ver_major = 0, ver_minor = 0;
                            if (reg_peer.has_version_major()) {
                                ver_major = reg_peer.version_major();
                            }
                            if (reg_peer.has_version_minor()) {
                                ver_minor = reg_peer.version_minor();
                            }

                            // From now on we know that this ucs is reachable via this connection.
                            connection.ucs_id = new_peer;
                            if (dupe) {
                                LOG("Another connection from UCS %08X detected from %s",
                                    new_peer,
                                    connection.stream->Get_name().c_str());
                            } else {
                                // We have connection from new ucs.
                                connection.primary = true;

                                std::string version = std::to_string(ver_major) + "." + std::to_string(ver_minor);
                                if (reg_peer.has_version_build()) {
                                    version += ".";
                                    version += reg_peer.version_build();
                                }
                                LOG("New UCS %08X detected on %s, version: %s",
                                    new_peer,
                                    connection.stream->Get_name().c_str(),
                                    version.c_str());
                                // Activate transport_detector
                                Transport_detector::Get_instance()->Activate(true);
                            }

                            // We know that UCS below this version uses different protocol.
                            if (ver_major < SUPPORTED_UCS_VERSION_MAJOR ||
                                (ver_major == SUPPORTED_UCS_VERSION_MAJOR && ver_minor < SUPPORTED_UCS_VERSION_MINOR))
                            {
                                connection.is_compatible = false;
                                LOG("UCS %08X is incompatible with this VSM.", new_peer);
                            }

                            // Send all known vehicles.
                            Send_vehicle_registrations(connection);
                        } else {
                            // Invalid peer type.
                            LOG_WARN(
                                "connection from invalid peer_type: %d. VSM supports connections only from server.",
                                reg_peer.peer_type());
                            Close_ucs_stream(stream_id);
                            return;
                        }
                    } else {
                        LOG_WARN("Got message for device %d from unregistered peer. Dropped.", vsm_msg.device_id());
                    }
                }
            } else {
                LOG_ERR("ParseFromArray failed, closing.");
                Close_ucs_stream(stream_id);
                return;
            }
            // wait for next message header
            connection.to_read = 1;
            connection.reading_header = true;
            connection.message_size = 0;
        }
        Schedule_next_read(iter->second);
    } else {
        Close_ucs_stream(stream_id);
    }
}

void
Cucs_processor::On_register_vehicle(Request::Ptr request, Device::Ptr vehicle)
{
    auto device_id = vehicle->Get_session_id();
    if (vehicles.find(device_id) != vehicles.end()) {
        VSM_EXCEPTION(Exception, "Vehicle %d already registered", device_id);
    }

    auto res = vehicles.emplace(device_id, Vehicle_context());
    auto &ctx = res.first->second;
    ctx.vehicle = vehicle;

    ctx.registration_message.set_device_id(device_id);

    vehicle->Register(ctx.registration_message);

    // LOG("Vehicle registered %s",ctx.registration_message.SerializeAsString().c_str());

    request->Complete();

    Broadcast_message_to_ucs(ctx.registration_message);
}

void
Cucs_processor::Send_vehicle_registrations(
    Server_context& ctx)
{
    for (auto &it : vehicles) {
        Send_ucs_message(ctx.stream_id, it.second.registration_message);
    }
}

void
Cucs_processor::On_unregister_vehicle(Request::Ptr request, uint32_t device_id)
{
    auto it = vehicles.find(device_id);
    if (it == vehicles.end()) {
        VSM_EXCEPTION(Invalid_param_exception, "Unregister unknown device id %d", device_id);
    } else {
        ugcs::vsm::proto::Vsm_message reg;
        reg.set_device_id(device_id);
        reg.mutable_unregister_device();
        vehicles.erase(device_id);
        Broadcast_message_to_ucs(reg);
    }
    request->Complete();
}

void
Cucs_processor::On_send_ucs_message(
    Request::Ptr request,
    uint32_t device_id,
    Proto_msg_ptr message,
    uint32_t stream_id)
{
    auto it = vehicles.find(device_id);
    if (it != vehicles.end()) {
        for (int i = 0; i < message->device_status().telemetry_fields_size(); i++) {
            auto &f = message->device_status().telemetry_fields(i);
            auto ce = it->second.telemetry_cache.emplace(f.field_id(), f);
            if (!ce.second) {
                // update cache entry
                ce.first->second.CopyFrom(f);
            }
        }
        for (int i = 0; i < message->device_status().command_availability_size(); i++) {
            auto &f = message->device_status().command_availability(i);
            auto ce = it->second.availability_cache.emplace(f.id(), f);
            if (!ce.second) {
                // update cache entry
                ce.first->second.CopyFrom(f);
            }
        }
        message->set_device_id(device_id);
        if (stream_id) {
            Send_ucs_message(stream_id, *message);
        } else {
            Broadcast_message_to_ucs(*message);
        }
    } else {
        // This can happen if vehicle is removed while message is dispatched already.
        // Nothing deadly. Ignore.
    }
    request->Complete();
}

void
Cucs_processor::Broadcast_message_to_ucs(ugcs::vsm::proto::Vsm_message& message)
{
    for (auto& iter : ucs_connections) {
        // Broadcast only to primary connections.
        if (iter.second.primary) {
            Send_ucs_message(iter.first, message);
        }
    }
}

void
Cucs_processor::Send_ucs_message_ptr(
    uint32_t stream_id,
    Proto_msg_ptr message)
{
    Send_ucs_message(stream_id, *message);
}

void
Cucs_processor::Send_ucs_message(
    uint32_t stream_id,
    ugcs::vsm::proto::Vsm_message& message)
{
    auto iter = ucs_connections.find(stream_id);
    if (iter != ucs_connections.end()) {
        auto & ctx = iter->second;

        if (!ctx.ucs_id) {
            // only register_peer message is allowed to be sent to unknown ucs.
            if (!message.has_register_peer()) {
                LOG_ERR("Must register peer before sending anything else");
                return;
            }
            message.set_device_id(0);
        }

        if (!ctx.is_compatible) {
            return;
        }

        if (message.has_register_device()) {
            // Force response_required for Register_device.
            message.set_response_required(true);
            message.set_message_id(Get_next_id());
            ctx.pending_registrations.insert(std::make_pair(message.message_id(), message.device_id()));
        } else if (message.device_id() != 0) {
            // This is a message from some device.
            auto it = ctx.registered_devices.find(message.device_id());
            if (it == ctx.registered_devices.end()) {
                // Not sending if device is not registered with this connection.
                return;
            } else {
                if (message.has_unregister_device()) {
                    // clean device specific stuff from ctx on Unregister_device.
                    ctx.registered_devices.erase(message.device_id());
                    for (auto m : ctx.pending_registrations) {
                        if (m.second == message.device_id()) {
                            ctx.pending_registrations.erase(m.first);
                            break;
                        }
                    }
                }
            }
        }

        if (   !message.has_message_id()
            &&  message.has_response_required()
            &&  message.response_required())
        {
            message.set_message_id(Get_next_id());
        }

        int header_len = 0;
        auto payload_len = message.ByteSize();
        auto tmp_len = payload_len;
        std::vector<uint8_t> user_data(10 + payload_len);
        do {
            uint8_t byte = (tmp_len & 0x7f);
            tmp_len >>= 7;
            if (tmp_len) {
                byte |= 0x80;
            }
            user_data[header_len] = byte;
            header_len++;
        } while (tmp_len);
        message.SerializeToArray(user_data.data() + header_len, payload_len);
        user_data.resize(header_len + payload_len);
        Io_buffer::Ptr buffer = Io_buffer::Create(std::move(user_data));

        // LOG("sending msg: %s", message.SerializeAsString().c_str());
        // LOG("sending msg len: %d", header_len + payload_len);
        ctx.stream->Write(
                buffer,
                Make_write_callback(
                        &Cucs_processor::Write_completed,
                        Shared_from_this(),
                        stream_id),
                completion_ctx).Timeout(WRITE_TIMEOUT);
    }
}

void
Cucs_processor::Write_completed(
        Io_result result,
        size_t stream_id)
{
    if (result != Io_result::OK) {
        // Write failed. Assume connection dead.
        Close_ucs_stream(stream_id);
    }
}

void
Cucs_processor::Close_ucs_stream(size_t stream_id)
{
    auto iter = ucs_connections.find(stream_id);
    if (iter != ucs_connections.end()) {
        if (iter->second.ucs_id) {
        LOG("Closing UCS %08X connection %s",
            *iter->second.ucs_id,
            iter->second.address->Get_as_string().c_str());
        }
        iter->second.stream->Close();
        auto primary = iter->second.primary;
        uint32_t ucs_id = 0;
        if (iter->second.ucs_id) {
            ucs_id = *iter->second.ucs_id;
        }

        auto devices = iter->second.registered_devices;
        ucs_connections.erase(iter);

        if (primary) {
            // Primary connection erased.
            // Look if there is another loopback connection and make that primary.
            auto loopback_found = false;
            for (auto& ucs : ucs_connections) {
                if (ucs.second.ucs_id && *(ucs.second.ucs_id) == ucs_id) {
                    if (ucs.second.address->Is_loopback_address()) {
                        ucs.second.primary = true;
                        loopback_found = true;
                        LOG("New primary connection for UCS %08X: %s",
                            ucs_id,
                            ucs.second.address->Get_as_string().c_str());
                    }
                }
            }
            if (!loopback_found) {
                // No other loopback connection. Make primary whichever is found first.
                for (auto& ucs : ucs_connections) {
                    if (ucs.second.ucs_id && *(ucs.second.ucs_id) == ucs_id) {
                        ucs.second.primary = true;
                        LOG("New primary connection for UCS %08X: %s",
                            ucs_id,
                            ucs.second.address->Get_as_string().c_str());
                        break;
                    }
                }
            }
        }

        // For all devices registered with the closed connection we gather
        // all other connections the device is registered to.
        // And tell that to the device via Handle_ucs_info call.
        for (auto dev_id : devices) {
            Notify_device_about_ucs_connections(dev_id);
        }

        if (ucs_connections.size() == 0) {
            if (!transport_detector_on_when_diconnected) {
                Transport_detector::Get_instance()->Activate(false);
            }
        }
    }
}

void
Cucs_processor::Notify_device_about_ucs_connections(uint32_t dev_id)
{
    auto dev = Get_device(dev_id);
    if (dev) {
        std::vector<Ucs_info> ucs_data;
        for (auto& u : ucs_connections) {
            if (u.second.registered_devices.find(dev_id) != u.second.registered_devices.end()) {
                ucs_data.emplace_back(Ucs_info{
                    *u.second.ucs_id,
                    Socket_address::Create(u.second.address),
                    u.second.primary,
                    u.second.last_message_time});
            }
        }
        // Handle_ucs_info call must happen within device context.
        auto request = Request::Create();
        request->Set_processing_handler(
            Make_callback([](Request::Ptr r, Device::Ptr d, std::vector<Ucs_info> data) {
                d->Handle_ucs_info(data);
                r->Complete();
            },
            request,
            dev,
            ucs_data));
        dev->Get_processing_ctx()->Submit_request(request);
    }
}

Device::Ptr
Cucs_processor::Get_device(uint32_t device_id)
{
    auto it = vehicles.find(device_id);
    if (it == vehicles.end()) {
        return nullptr;
    }
    return it->second.vehicle;
}

void
Cucs_processor::On_ucs_message(
    uint32_t stream_id,
    ugcs::vsm::proto::Vsm_message message)
{
    auto dev_id = message.device_id();
    auto dev = Get_device(dev_id);
    if (message.has_response_required() && message.response_required()) {
        // ucs will wait for response on this message.
        // Prepare the response template and set up completion handler.
        // Need this to send the response into the same connection as request.
        auto resp = std::make_shared<ugcs::vsm::proto::Vsm_message>();
        resp->set_message_id(message.message_id());
        resp->set_device_id(dev_id);
        if (dev) {
            // by default assume failure
            resp->mutable_device_response()->set_code(ugcs::vsm::proto::STATUS_FAILED);
            // pass response message template to vehicle, default to failed.
            auto completion_handler = Make_callback(
                &Cucs_processor::Send_ucs_message_ptr,
                Shared_from_this(),
                stream_id,
                resp);

            dev->On_ucs_message(
                std::move(message),
                completion_handler,
                completion_ctx);
            return;     // completion handler will send the response.
        } else {
            if (dev_id) {
                resp->mutable_device_response()->set_code(ugcs::vsm::proto::STATUS_INVALID_SESSION_ID);
                LOG_ERR("Received message for unknown device %d", dev_id);
            } else {
                // Respond OK to any request for the peer itself.
                resp->mutable_device_response()->set_code(ugcs::vsm::proto::STATUS_OK);
            }
        }
        // Message not passed to vehicle. Send response.
        Send_ucs_message_ptr(stream_id, resp);
    } else {
        // No response required.
        if (dev) {
            // Call vehicle handler. No completion handler needed.
            dev->On_ucs_message(std::move(message));
        } else {
            if (dev_id) {
                LOG_ERR("Received message for unknown vehicle %d", dev_id);
            } // else ignore messages destined for this peer.
        }
    }
}
