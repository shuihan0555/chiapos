// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_CPP_DISK_HPP_
#define SRC_CPP_DISK_HPP_

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/uio.h>

// enables disk I/O logging to disk.log
// use tools/disk.gnuplot to generate a plot
#define ENABLE_LOGGING 0

using namespace std::chrono_literals; // for operator""min;

#include "chia_filesystem.hpp"

#include "./bits.hpp"
#include "./util.hpp"
#include "bitfield.hpp"
#include "concurrentqueue/concurrentqueue.h"

constexpr uint64_t write_cache = 1024 * 1024;
constexpr uint64_t read_ahead = 1024 * 1024;

struct Disk {
    virtual uint8_t const* Read(uint64_t begin, uint64_t length) = 0;
    virtual void Write(uint64_t begin, const uint8_t *memcache, uint64_t length) = 0;
    virtual void Truncate(uint64_t new_size) = 0;
    virtual std::string GetFileName() = 0;
    virtual void FreeMemory() = 0;
    virtual ~Disk() = default;
};

#if ENABLE_LOGGING
// logging is currently unix / bsd only: use <fstream> or update
// calls to ::open and ::write to port to windows
#include <fcntl.h>
#include <unistd.h>
#include <mutex>
#include <unordered_map>
#include <cinttypes>

enum class op_t : int { read, write};

void disk_log(fs::path const& filename, op_t const op, uint64_t offset, uint64_t length)
{
    static std::mutex m;
    static std::unordered_map<std::string, int> file_index;
    static auto const start_time = std::chrono::steady_clock::now();
    static int next_file = 0;

    auto const timestamp = std::chrono::steady_clock::now() - start_time;

    int fd = ::open("disk.log", O_WRONLY | O_CREAT | O_APPEND, 0755);

    std::unique_lock<std::mutex> l(m);

    char buffer[512];

    int const index = [&] {
        auto it = file_index.find(filename.string());
        if (it != file_index.end()) return it->second;
        file_index[filename.string()] = next_file;

        int const len = std::snprintf(buffer, sizeof(buffer)
            , "# %d %s\n", next_file, filename.string().c_str());
        ::write(fd, buffer, len);
        return next_file++;
    }();

    // timestamp (ms), start-offset, end-offset, operation (0 = read, 1 = write), file_index
    int const len = std::snprintf(buffer, sizeof(buffer)
        , "%" PRId64 "\t%" PRIu64 "\t%" PRIu64 "\t%d\t%d\n"
        , std::chrono::duration_cast<std::chrono::milliseconds>(timestamp).count()
        , offset
        , offset + length
        , int(op)
        , index);
    ::write(fd, buffer, len);
    ::close(fd);
}
#endif

struct BufferedDiskMeta {
	uint8_t* data;
	uint64_t length;
	uint64_t begin;
	BufferedDiskMeta() {
		data = nullptr;
		length = 0;
		begin = 0;
	}

	BufferedDiskMeta(const uint8_t* buff, uint64_t length, uint64_t begin) {
		data = (uint8_t*)malloc(length);
		memcpy(data, buff, length);
		this->length = length;
		this->begin = begin;
	}

	BufferedDiskMeta(const BufferedDiskMeta& r) = delete;
	BufferedDiskMeta& operator=(const BufferedDiskMeta& r) = delete;
	BufferedDiskMeta(BufferedDiskMeta&& r) {
		data = r.data;
		length = r.length;
		begin = r.begin;
		r.data = nullptr;
		r.begin = 0;
	}

	BufferedDiskMeta& operator=(BufferedDiskMeta&& r) {
		if (this == &r) {
			return *this;
		}
		if (data != nullptr) {
			free(data);
		}

		data = r.data;
		length = r.length;
		begin = r.begin;
		r.data = nullptr;
		r.length = 0;
		r.begin = 0;

		return *this;
	}

	~BufferedDiskMeta() {
		if (data != nullptr) {
			free(data);
			data = nullptr;
			length = 0;
			begin = 0;
		}
	}
};

struct FileDisk {
#if !defined(FREE_WRITE)
    explicit FileDisk(const fs::path &filename)
    {
        filename_ = filename;
        Open(writeFlag);
		async_ = false;
    }
#else
    explicit FileDisk(const fs::path &filename, bool async=true)
    {
        filename_ = filename;
		async_ = async;
        Open(writeFlag);
		if (async_) {
			std::thread t(&FileDisk::Async, this);
			thread_queue_= std::move(t);
		}
    }
#endif

    void Open(uint8_t flags = 0)
    {
        // if the file is already open, don't do anything
        if (f_) return;

        // Opens the file for reading and writing
        do {
#ifdef _WIN32
            f_ = ::_wfopen(filename_.c_str(), (flags & writeFlag) ? L"w+b" : L"r+b");
#else
            f_ = ::fopen(filename_.c_str(), (flags & writeFlag) ? "w+b" : "r+b");
#endif
            if (f_ == nullptr) {
                std::string error_message =
                    "Could not open " + filename_.string() + ": " + ::strerror(errno) + ".";
                if (flags & retryOpenFlag) {
                    std::cout << error_message << " Retrying in five minutes." << std::endl;
                    std::this_thread::sleep_for(5min);
                } else {
                    throw InvalidValueException(error_message);
                }
            }
        } while (f_ == nullptr);
    }

    FileDisk(FileDisk &&fd)
    {
        filename_ = std::move(fd.filename_);
        f_ = fd.f_;
        fd.f_ = nullptr;
    }

    FileDisk(const FileDisk &) = delete;
    FileDisk &operator=(const FileDisk &) = delete;

    void Close()
    {
        if (f_ == nullptr) return;
        ::fclose(f_);
        f_ = nullptr;
        readPos = 0;
        writePos = 0;
    }

    ~FileDisk() { 
		if (async_) {
			thread_queue_.join();
		}
		Close(); 
	}

    void Read(uint64_t begin, uint8_t *memcache, uint64_t length)
    {
        Open(retryOpenFlag);
#if ENABLE_LOGGING
        disk_log(filename_, op_t::read, begin, length);
#endif
        // Seek, read, and replace into memcache
        uint64_t amtread;
        do {
            if ((!bReading) || (begin != readPos)) {
#ifdef _WIN32
                _fseeki64(f_, begin, SEEK_SET);
#else
                // fseek() takes a long as offset, make sure it's wide enough
                static_assert(sizeof(long) >= sizeof(begin));
                ::fseek(f_, begin, SEEK_SET);
#endif
                bReading = true;
            }
            amtread = ::fread(reinterpret_cast<char *>(memcache), sizeof(uint8_t), length, f_);
            readPos = begin + amtread;
            if (amtread != length) {
                std::cout << "Only read " << amtread << " of " << length << " bytes at offset "
                          << begin << " from " << filename_ << " with length " << writeMax
                          << ". Error " << ferror(f_) << ". Retrying in five minutes." << std::endl;
                // Close, Reopen, and re-seek the file to recover in case the filesystem
                // has been remounted.
                Close();
                bReading = false;
                std::this_thread::sleep_for(5min);
                Open(retryOpenFlag);
            }
        } while (amtread != length);
    }

#if !defined(FREE_WRITE)
    void Write(uint64_t begin, const uint8_t *memcache, uint64_t length)
    {
        Open(writeFlag | retryOpenFlag);
#if ENABLE_LOGGING
        disk_log(filename_, op_t::write, begin, length);
#endif
        // Seek and write from memcache
        uint64_t amtwritten;
        do {
            if ((bReading) || (begin != writePos)) {
#ifdef _WIN32
                _fseeki64(f_, begin, SEEK_SET);
#else
                // fseek() takes a long as offset, make sure it's wide enough
                static_assert(sizeof(long) >= sizeof(begin));
                ::fseek(f_, begin, SEEK_SET);
#endif
                bReading = false;
            }
            amtwritten =
                ::fwrite(reinterpret_cast<const char *>(memcache), sizeof(uint8_t), length, f_);
            writePos = begin + amtwritten;
            if (writePos > writeMax)
                writeMax = writePos;
            if (amtwritten != length) {
                // If an error occurs, the resulting value of the file-position indicator for the stream is unspecified.
                // https://pubs.opengroup.org/onlinepubs/007904975/functions/fwrite.html
                //
                // And in the code above if error occurs with 0 bytes written (full disk) it will not reset the pointer
                // (writePos will still be equal to begin), however it need to be reseted.
                //
                // Otherwise this causes #234 - in phase3, when this bucket is read, it goes into endless loop.
                //
                // Thanks tinodj!
                writePos = UINT64_MAX;
                std::cout << "Only wrote " << amtwritten << " of " << length << " bytes at offset "
                          << begin << " to " << filename_ << " with length " << writeMax
                          << ". Error " << ferror(f_) << ". Retrying in five minutes." << std::endl;
                // Close, Reopen, and re-seek the file to recover in case the filesystem
                // has been remounted.
                Close();
                bReading = false;
                std::this_thread::sleep_for(5min);
                Open(writeFlag | retryOpenFlag);
            }
        } while (amtwritten != length);
    }

#else
    void Write(uint64_t begin, const uint8_t *memcache, uint64_t length)
    {
		//BufferedDiskMeta meta(memcache, length, begin);
		BufferedDiskMeta*  meta = new BufferedDiskMeta(memcache, length, begin);
		//queue_.enqueue(std::move(meta));
		queue_.enqueue(meta);
	}
#endif

	void SetRun() {
		run_ = 0;
	}

	void Async() 
	{
		run_ = 1;
		do {
			BufferedDiskMeta* items[BULK_SIZE] = {};
			struct iovec iov[BULK_SIZE];
			int count = 0;
			do {
				size_t len = 0;
				count = queue_.try_dequeue_bulk(items, BULK_SIZE);
				int i = 0;
				for(; i < count; ++i) {
					if (len + items[i]->length >= LONG_MAX) {
						break;
					} else {
						len +=items[i]->length;
						iov[i].iov_base = (void*) items[i]->data;
						iov[i].iov_len = items[i]->length;
					}
				}


				Open(writeFlag | retryOpenFlag);
#if ENABLE_LOGGING
				disk_log(filename_, op_t::write, begin, length);
#endif
				uint64_t amtwritten;
				int cnt = 0;
				do {
					if ((bReading) || (items[0]->begin != writePos)) {
#ifdef _WIN32
						_fseeki64(f_, items[0]->begin, SEEK_SET);
#else
						// fseek() takes a long as offset, make sure it's wide enough
						//static_assert(sizeof(long) >= sizeof(begin));
						::fseek(f_, items[0]->begin, SEEK_SET);
#endif
						bReading = false;
					}
					//amtwritten =::fwrite(reinterpret_cast<const char *>(memcache), sizeof(uint8_t), length, f_);
					int fd = fileno(f_);
					amtwritten = pwritev(fd, iov, i, items[0]->begin);
					if (amtwritten != len) {
						//std::cout << "Only wrote " << amtwritten << " of " << length << " bytes at offset "
						//	<< begin << " to " << filename_ << " with length " << writeMax
						//	<< ". Error " << ferror(f_) << ". Retrying in five minutes." << std::endl;
						// Close, Reopen, and re-seek the file to recover in case the filesystem
						// has been remounted.
						Close();
						bReading = false;
						std::this_thread::sleep_for(1min);
						Open(writeFlag | retryOpenFlag);
					} else {
						writePos += amtwritten;
						break;
					}
				} while(true);

				for (; i<count; ++i) 
				{
					do {
						Open(writeFlag | retryOpenFlag);
						if ((bReading) || (items[i]->begin != writePos)) {
#ifdef _WIN32
							_fseeki64(f_, items[i]->begin, SEEK_SET);
#else
							// fseek() takes a long as offset, make sure it's wide enough
							//static_assert(sizeof(long) >= sizeof(begin));
							::fseek(f_, items[i]->begin, SEEK_SET);
#endif
							bReading = false;
						}
						int fd = fileno(f_);
						amtwritten = pwritev(fd, &iov[i], 1, items[i]->begin);
						if (amtwritten != items[i]->length) {
							std::cout << "Only wrote " << amtwritten << " of  bytes at offset "
								<< ". Error " << ferror(f_) << ". Retrying in five minutes." << std::endl;
							// Close, Reopen, and re-seek the file to recover in case the filesystem
							// has been remounted.
							Close();
							bReading = false;
							std::this_thread::sleep_for(1min);
							Open(writeFlag | retryOpenFlag);
						} else {
							writePos += items[i]->length;
							break;
						}
					} while(true);
				}

				bool finish = false;
				if (items[i]->length == 0) {
					finish = true;
				}

				for (int k=0; k<count; ++k) {
					delete items[k];
				}

				if (finish) {
					return;
				}

				if(count < BULK_SIZE){
					break;
				}
			} while(true);
			sleep(5);
		}while(run_);
    }

    std::string GetFileName() { return filename_.string(); }

    uint64_t GetWriteMax() const noexcept { return writeMax; }

    void Truncate(uint64_t new_size)
    {
        Close();
        fs::resize_file(filename_, new_size);
    }

	size_t GetSize() const {
		return queue_.size_approx();
	}

private:

    uint64_t readPos = 0;
    uint64_t writePos = 0;
    uint64_t writeMax = 0;
    bool bReading = true;

    fs::path filename_;
    FILE *f_ = nullptr;

    static const uint8_t writeFlag = 0b01;
    static const uint8_t retryOpenFlag = 0b10;

	moodycamel::ConcurrentQueue<BufferedDiskMeta*> queue_;
	std::thread thread_queue_;
	const int BULK_SIZE = 100;
	std::atomic<int> run_;
	bool async_ = false;
};


struct BufferedDisk : Disk
{
    BufferedDisk(FileDisk* disk, uint64_t file_size) : disk_(disk), file_size_(file_size) {}

    uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
        assert(length < read_ahead);
        NeedReadCache();
        // all allocations need 7 bytes head-room, since
        // SliceInt64FromBytes() may overrun by 7 bytes
        if (read_buffer_start_ <= begin
            && read_buffer_start_ + read_buffer_size_ >= begin + length
            && read_buffer_start_ + read_ahead >= begin + length + 7)
        {
            // if the read is entirely inside the buffer, just return it
            return read_buffer_.get() + (begin - read_buffer_start_);
        }
        else if (begin >= read_buffer_start_ || begin == 0 || read_buffer_start_ == std::uint64_t(-1)) {

            // if the read is beyond the current buffer (i.e.
            // forward-sequential) move the buffer forward and read the next
            // buffer-capacity number of bytes.
            // this is also the case we enter the first time we perform a read,
            // where we haven't read anything into the buffer yet. Note that
            // begin == 0 won't reliably detect that case, sinec we may have
            // discarded the first entry and start at some low offset but still
            // greater than 0
            read_buffer_start_ = begin;
            uint64_t const amount_to_read = std::min(file_size_ - read_buffer_start_, read_ahead);
            disk_->Read(begin, read_buffer_.get(), amount_to_read);
            read_buffer_size_ = amount_to_read;
            return read_buffer_.get();
        }
        else {
            // ideally this won't happen
            std::cout << "Disk read position regressed. It's optimized for forward scans. Performance may suffer\n"
                << "   read-offset: " << begin
                << " read-length: " << length
                << " file-size: " << file_size_
                << " read-buffer: [" << read_buffer_start_ << ", " << read_buffer_size_ << "]"
                << " file: " << disk_->GetFileName()
                << '\n';
            static uint8_t temp[128];
            // all allocations need 7 bytes head-room, since
            // SliceInt64FromBytes() may overrun by 7 bytes
            assert(length <= sizeof(temp) - 7);

            // if we're going backwards, don't wipe out the cache. We assume
            // forward sequential access
            disk_->Read(begin, temp, length);
            return temp;
        }
    }

#if !defined(FREE_WRITE)
    void Write(uint64_t const begin, const uint8_t *memcache, uint64_t const length) override
    {
        NeedWriteCache();
        if (begin == write_buffer_start_ + write_buffer_size_) {
            if (write_buffer_size_ + length <= write_cache) {
                ::memcpy(write_buffer_.get() + write_buffer_size_, memcache, length);
                write_buffer_size_ += length;
                return;
            }
            FlushCache();
        }

        if (write_buffer_size_ == 0 && write_cache >= length) {
            write_buffer_start_ = begin;
            ::memcpy(write_buffer_.get() + write_buffer_size_, memcache, length);
            write_buffer_size_ = length;
            return;
        }

        disk_->Write(begin, memcache, length);
    }
#else
    void Write(uint64_t const begin, const uint8_t *memcache, uint64_t const length) override
	{
        disk_->Write(begin, memcache, length);
	}
#endif

    void Truncate(uint64_t const new_size) override
    {
        FlushCache();
        disk_->Truncate(new_size);
        file_size_ = new_size;
        FreeMemory();
    }

    std::string GetFileName() override { return disk_->GetFileName(); }

    void FreeMemory() override
    {
        FlushCache();

        read_buffer_.reset();
        write_buffer_.reset();
        read_buffer_size_ = 0;
        write_buffer_size_ = 0;
    }

#if !defined(FREE_WRITE)
    void FlushCache()
    {
        if (write_buffer_size_ == 0) return;

        disk_->Write(write_buffer_start_, write_buffer_.get(), write_buffer_size_);
        write_buffer_size_ = 0;
    }
#else
    void FlushCache()
    {
		int cnt = 3;
		do {
			if (disk_->GetSize()==0) {
				usleep(20);
			} else {
				sleep(6);
			}
		} while(--cnt);

		disk_->SetRun();
    }
#endif

private:

    void NeedReadCache()
    {
        if (read_buffer_) return;
        read_buffer_.reset(new uint8_t[read_ahead]);
        read_buffer_start_ = -1;
        read_buffer_size_ = 0;
    }

    void NeedWriteCache()
    {
        if (write_buffer_) return;
        write_buffer_.reset(new uint8_t[write_cache]);
        write_buffer_start_ = -1;
        write_buffer_size_ = 0;
    }

    FileDisk* disk_;

    uint64_t file_size_;

    // the file offset the read buffer was read from
    uint64_t read_buffer_start_ = -1;
    std::unique_ptr<uint8_t[]> read_buffer_;
    uint64_t read_buffer_size_ = 0;

    // the file offset the write buffer should be written back to
    // the write buffer is *only* for contiguous and sequential writes
    uint64_t write_buffer_start_ = -1;
    std::unique_ptr<uint8_t[]> write_buffer_;
    uint64_t write_buffer_size_ = 0;
};

struct FilteredDisk : Disk
{
    FilteredDisk(BufferedDisk underlying, bitfield filter, int entry_size)
        : filter_(std::move(filter))
        , underlying_(std::move(underlying))
        , entry_size_(entry_size)
    {
        assert(entry_size_ > 0);
        while (!filter_.get(last_idx_)) {
            last_physical_ += entry_size_;
            ++last_idx_;
        }
        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);
    }

    uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
        // we only support a single read-pass with no going backwards
        assert(begin >= last_logical_);
        assert((begin % entry_size_) == 0);
        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);

        if (begin > last_logical_) {
            // last_idx_ et.al. always points to an entry we have (i.e. the bit
            // is set). So when we advance from there, we always take at least
            // one step on all counters.
            last_logical_ += entry_size_;
            last_physical_ += entry_size_;
            ++last_idx_;

            while (begin > last_logical_)
            {
                if (filter_.get(last_idx_)) {
                    last_logical_ += entry_size_;
                }
                last_physical_ += entry_size_;
                ++last_idx_;
            }

            while (!filter_.get(last_idx_)) {
                last_physical_ += entry_size_;
                ++last_idx_;
            }
        }

        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);
        assert(begin == last_logical_);
        return underlying_.Read(last_physical_, length);
    }

    void Write(uint64_t begin, const uint8_t *memcache, uint64_t length) override
    {
        assert(false);
        throw std::runtime_error("Write() called on read-only disk abstraction");
    }
    void Truncate(uint64_t new_size) override
    {
        underlying_.Truncate(new_size);
        if (new_size == 0) filter_.free_memory();
    }
    std::string GetFileName() override { return underlying_.GetFileName(); }
    void FreeMemory() override
    {
        filter_.free_memory();
        underlying_.FreeMemory();
    }

private:

    // only entries whose bit is set should be read
    bitfield filter_;
    BufferedDisk underlying_;
    int entry_size_;

    // the "physical" disk offset of the last read
    uint64_t last_physical_ = 0;
    // the "logical" disk offset of the last read. i.e. the offset as if the
    // file would have been compacted based on filter_
    uint64_t last_logical_ = 0;

    // the index of the last read. This is also the index into the bitfield. It
    // could be computed as last_physical_ / entry_size_, but we want to avoid
    // the division.
    uint64_t last_idx_ = 0;
};

#endif  // SRC_CPP_DISK_HPP_
