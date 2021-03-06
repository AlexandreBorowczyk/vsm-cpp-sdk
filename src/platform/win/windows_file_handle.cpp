#include <ugcs/vsm/windows_file_handle.h>
#include <ugcs/vsm/windows_wstring.h>

using namespace ugcs::vsm::internal;
using namespace ugcs::vsm;

Windows_file_handle::Windows_file_handle(HANDLE handle, HANDLE write_handle) :
		handle(handle), write_handle(write_handle)
{

}

Windows_file_handle::Windows_file_handle(
    const std::string &path, File_processor::Stream::Mode mode)
{
    DWORD access = 0, creation = 0, share_mode = 0;

    if (mode.read) {
        access = GENERIC_READ;
        share_mode = FILE_SHARE_READ;
        if (mode.extended) {
            access |= GENERIC_WRITE;
        } else {
            share_mode |= FILE_SHARE_WRITE;
        }
        if (mode.should_not_exist) {
            creation = OPEN_ALWAYS;
        } else {
            creation = OPEN_EXISTING;
        }
    } else if (mode.write) {
        access = GENERIC_WRITE;
        share_mode = FILE_SHARE_READ;
        if (mode.extended) {
            access |= GENERIC_READ;
        }
        if (mode.should_not_exist) {
            creation = CREATE_NEW;
        } else {
            creation = CREATE_ALWAYS;
        }
    } else {
        ASSERT(false);
    }

    try {
        this->handle = CreateFileW(Windows_wstring(path), access, share_mode,
                nullptr, creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                0);
    } catch (const Windows_wstring::Conversion_failure&) {
        VSM_EXCEPTION(File_processor::Exception,
                "Failed to convert file name to wide character string: %s",
                path.c_str());
    }

    if (this->handle == INVALID_HANDLE_VALUE) {
        switch (GetLastError()) {
        case ERROR_ACCESS_DENIED:
            VSM_EXCEPTION(File_processor::Permission_denied_exception,
                          "Insufficient permissions for file opening: %s",
                          path.c_str());
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            VSM_EXCEPTION(File_processor::Already_exists_exception,
                          "File already exists: %s", path.c_str());
        case ERROR_FILE_NOT_FOUND:
            VSM_EXCEPTION(File_processor::Not_found_exception,
                          "File not found: %s", path.c_str());
        default:
            VSM_EXCEPTION(File_processor::Exception,
                          "Failed to open file '%s': %s", path.c_str(),
                          Log::Get_system_error().c_str());
        }
    }
}

Windows_file_handle::~Windows_file_handle()
{
    if (!is_closed) {
        Close();
    }
    CloseHandle(lock_complete_event);
}

Io_result
Windows_file_handle::Map_error(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS:
        return Io_result::OK;
    case ERROR_ACCESS_DENIED:
        return Io_result::PERMISSION_DENIED;
    case ERROR_HANDLE_EOF:
        return Io_result::END_OF_FILE;
    case ERROR_OPERATION_ABORTED:
        return Io_result::CANCELED;
    case ERROR_INVALID_HANDLE:
        return Io_result::CLOSED;
    default:
        return Io_result::OTHER_FAILURE;
    }
}

void
Windows_file_handle::Write()
{
    std::unique_lock<std::mutex> lock(write_mutex);

    std::memset(&write_cb, 0, sizeof(write_cb));
    write_offset = cur_write_request->Offset();
    if (write_offset == Io_stream::OFFSET_NONE) {
        write_cb.Offset = 0;
        write_cb.OffsetHigh = 0;
    } else {
        write_cb.Offset = write_offset;
        write_cb.OffsetHigh = write_offset >> sizeof(write_cb.Offset) * 8;
    }
    write_size = cur_write_request->Data_buffer()->Get_length();

    Set_write_activity(true);

    if (!WriteFile(write_handle == INVALID_HANDLE_VALUE ? handle : write_handle,
                   cur_write_request->Data_buffer()->Get_data(),
                   write_size, nullptr, &write_cb) &&
        GetLastError() != ERROR_IO_PENDING) {

        DWORD error = GetLastError();
        LOG_ERROR("WriteFile failed: %s", Log::Get_system_error().c_str());
        cur_write_request->Set_result_arg(Map_error(error));
        cur_write_request->Complete();
        Set_write_activity(false, std::move(lock));
    }
}

void
Windows_file_handle::Write_complete_cbk(size_t transfer_size, DWORD error)
{
    std::unique_lock<std::mutex> lock(write_mutex);

    if (cur_write_request->Get_status() != Request::Status::PROCESSING) {
        /* Canceled, no further processing required. */
        Handle_write_abort();
        Set_write_activity(false, std::move(lock));
        return;
    }

    if (is_closed) {
        cur_write_request->Set_result_arg(Io_result::CLOSED);
        cur_write_request->Complete();
        Set_write_activity(false, std::move(lock));
        return;
    }

    Io_result result;
    if (error) {
        result = Map_error(error);
    } else if (transfer_size < write_size) {
        /* Incomplete write, schedule the rest. */
        memset(&write_cb, 0, sizeof(write_cb));
        if (write_offset != Io_stream::OFFSET_NONE) {
            write_offset += transfer_size;
        }
        write_size -= transfer_size;
        if (write_offset == Io_stream::OFFSET_NONE) {
            write_cb.Offset = 0;
            write_cb.OffsetHigh = 0;
        } else {
            write_cb.Offset = write_offset;
            write_cb.OffsetHigh = write_offset >> sizeof(write_cb.Offset) * 8;
        }
        const void *buf =
            reinterpret_cast<const uint8_t *>(cur_write_request->Data_buffer()->Get_data()) +
            (cur_write_request->Data_buffer()->Get_length() - write_size);
        if (!WriteFile(write_handle == INVALID_HANDLE_VALUE ? handle : write_handle,
                       buf, write_size, nullptr, &write_cb) &&
            GetLastError() != ERROR_IO_PENDING) {

            DWORD error = GetLastError();
            LOG_ERROR("WriteFile failed (continuation): %s",
                      Log::Get_system_error().c_str());
            result = Map_error(error);
        } else {
            /* Write for the rest data is scheduled. */
            return;
        }
    } else {
        /* Operation successfully completed. */
        result = Io_result::OK;
    }
    cur_write_request->Set_result_arg(result);
    cur_write_request->Complete();
    Set_write_activity(false, std::move(lock));
}

void
Windows_file_handle::Read()
{
    std::unique_lock<std::mutex> lock(read_mutex);

    std::memset(&read_cb, 0, sizeof(read_cb));
    read_offset = cur_read_request->Offset();
    if (read_offset == Io_stream::OFFSET_NONE) {
        read_cb.Offset = 0;
        read_cb.OffsetHigh = 0;
    } else {
        read_cb.Offset = read_offset;
        read_cb.OffsetHigh = read_offset >> sizeof(read_cb.Offset) * 8;
    }
    read_size = cur_read_request->Get_max_to_read();
    min_read_size = cur_read_request->Get_min_to_read();
    read_buf = std::make_shared<std::vector<uint8_t>>(read_size);

    Set_read_activity(true);

    if (!ReadFile(handle, &read_buf->front(), read_size, nullptr, &read_cb) &&
        GetLastError() != ERROR_IO_PENDING) {

        DWORD error = GetLastError();
        LOG_ERROR("ReadFile failed: %s", Log::Get_system_error().c_str());
        cur_read_request->Set_result_arg(Map_error(error));
        if (error == ERROR_HANDLE_EOF) {
            read_buf->resize(read_buf->size() - read_size);
            cur_read_request->Set_buffer_arg(Io_buffer::Create(std::move(read_buf)));
        }
        cur_read_request->Complete();
        Set_read_activity(false, std::move(lock));
    }
}

void
Windows_file_handle::Lock_complete_cbk(DWORD error)
{
    lock_complete_result = error;
    SetEvent(lock_complete_event);
}

void
Windows_file_handle::Read_complete_cbk(size_t transfer_size, DWORD error)
{
    std::unique_lock<std::mutex> lock(read_mutex);

    if (cur_read_request->Get_status() != Request::Status::PROCESSING) {
        /* Canceled, no further processing required. */
        Handle_read_abort();
        Set_read_activity(false, std::move(lock));
        return;
    }

    if (is_closed) {
        cur_read_request->Set_result_arg(Io_result::CLOSED);
        cur_read_request->Complete();
        Set_read_activity(false, std::move(lock));
        return;
    }

    Io_result result;
    if (error) {
        result = Map_error(error);
        if (result == Io_result::END_OF_FILE) {
            read_buf->resize(read_buf->size() - read_size);
            cur_read_request->Set_buffer_arg(Io_buffer::Create(std::move(read_buf)));
        }
    } else if (transfer_size < min_read_size) {
        /* Incomplete read, schedule the rest. */
        memset(&read_cb, 0, sizeof(read_cb));
        if (read_offset != Io_stream::OFFSET_NONE) {
            read_offset += transfer_size;
        }
        read_size -= transfer_size;
        min_read_size -= transfer_size;
        if (read_offset == Io_stream::OFFSET_NONE) {
            read_cb.Offset = 0;
            read_cb.OffsetHigh = 0;
        } else {
            read_cb.Offset = read_offset;
            read_cb.OffsetHigh = read_offset >> sizeof(read_cb.Offset) * 8;
        }
        void *buf = &(*read_buf)[cur_read_request->Get_max_to_read() - read_size];
        if (!ReadFile(handle, buf, read_size, nullptr, &read_cb) &&
            GetLastError() != ERROR_IO_PENDING) {

            DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) {
                read_buf->resize(read_buf->size() - read_size);
                cur_read_request->Set_buffer_arg(Io_buffer::Create(std::move(read_buf)));
            }
            LOG_ERROR("ReadFile failed (continuation): %s",
                      Log::Get_system_error().c_str());
            result = Map_error(error);
        } else {
            /* Read for the rest data is scheduled. */
            return;
        }
    } else {
        /* Operation successfully completed. */
        result = Io_result::OK;
        read_size -= transfer_size;
        read_buf->resize(read_buf->size() - read_size);
        cur_read_request->Set_buffer_arg(Io_buffer::Create(std::move(read_buf)));
    }
    cur_read_request->Set_result_arg(result);
    cur_read_request->Complete();
    Set_read_activity(false, std::move(lock));
}

File_processor::Stream::Lock_result
Windows_file_handle::Try_lock()
{
    if (LockFile(handle, 0, 0, 1, 0)) {
        return File_processor::Stream::Lock_result::OK;
    } else {
        if (GetLastError() == ERROR_LOCK_VIOLATION) {
            return File_processor::Stream::Lock_result::BLOCKED;
        }
    }
    return File_processor::Stream::Lock_result::ERROR;
}

bool
Windows_file_handle::Lock()
{
    bool ret = false;
    memset(&lock_cb, 0, sizeof(lock_cb));
    if (lock_complete_event == INVALID_HANDLE_VALUE) {
        lock_complete_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (lock_complete_event == NULL) {
            VSM_SYS_EXCEPTION("CreateEvent failed");
        }
    }

    ResetEvent(lock_complete_event);
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &lock_cb)) {
        ret = true;
    } else {
        if (GetLastError() == ERROR_IO_PENDING) {
            /* Wait for separate event because handle is opened for async io
             * and completion port will receive the signal on our lock_cb.
             * event is set by Lock_complete_cbk()
             */
            if (WaitForSingleObject(lock_complete_event, INFINITE) == WAIT_OBJECT_0) {
                if (lock_complete_result == ERROR_SUCCESS) {
                    ret = true;
                }
            } else {
                VSM_SYS_EXCEPTION("WaitForSingleObject failed");
            }
        }
    }
    return ret;
}

bool
Windows_file_handle::Unlock()
{
    return UnlockFile(handle, 0, 0, 1, 0);
}

void
Windows_file_handle::Cancel_io(bool write)
{
    if ((!write || write_handle == INVALID_HANDLE_VALUE) && !CancelIo(handle)) {
        LOG_ERROR("CancelIo failed: %s",
                  Log::Get_system_error().c_str());
    }
    if (write && (write_handle != INVALID_HANDLE_VALUE) &&
        !CancelIo(write_handle)) {

        LOG_ERROR("CancelIo failed: %s",
                  Log::Get_system_error().c_str());
    }
}

bool
Windows_file_handle::Cancel_write()
{
    std::unique_lock<std::mutex> lock(write_mutex);
    Cancel_io(true);
    /* Windows never guarantees that I/O is canceled so still wait for
     * completion notification.
     */
    return false;
}

bool
Windows_file_handle::Cancel_read()
{
    std::unique_lock<std::mutex> lock(read_mutex);
    Cancel_io(false);
    /* Windows never guarantees that I/O is canceled so still wait for
     * completion notification.
     */
    return false;
}

void
Windows_file_handle::Io_complete_cbk(OVERLAPPED *io_cb, size_t transfer_size,
                                     DWORD error)
{
    if (io_cb == &write_cb) {
        Write_complete_cbk(transfer_size, error);
    } else if (io_cb == &read_cb) {
        Read_complete_cbk(transfer_size, error);
    } else if (io_cb == &lock_cb) {
        Lock_complete_cbk(error);
    } else {
        LOG_ERROR("Invalid I/O control block received");
    }
}

void
Windows_file_handle::Close()
{
    std::lock(read_mutex, write_mutex);

    std::unique_lock<std::mutex> rl(read_mutex, std::adopt_lock);
    std::unique_lock<std::mutex> wl(write_mutex, std::adopt_lock);

    is_closed = true;
    /* Any pending I/O should receive notification after that and release the
     * stream.
     */
    CancelIo(handle);
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    if (write_handle != INVALID_HANDLE_VALUE) {
        CancelIo(write_handle);
        CloseHandle(write_handle);
        write_handle = INVALID_HANDLE_VALUE;
    }
    stream = nullptr;
    /* Current requests are aborted by caller. */
}
