#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// A fixed-capacity, thread-safe byte ring buffer. Useful for decoupling a
// producer thread (e.g. a GStreamer audio callback) from a consumer thread
// (e.g. an audio output device callback that pulls PCM bytes).
class AudioRingBuffer
{
public:
    explicit AudioRingBuffer(size_t capacity)
        : buffer(capacity == 0 ? 1 : capacity), head(0), tail(0), size(0) {}

    // Producer side: deposit up to `bytesToWrite` bytes. Returns the number of
    // bytes actually written (less than requested when the buffer is full).
    size_t write(const uint8_t *data, size_t bytesToWrite)
    {
        if (data == nullptr || bytesToWrite == 0)
            return 0;

        std::lock_guard<std::mutex> lock(mtx);

        if (bytesToWrite > capacity())
        {
            bytesToWrite = capacity();
        }
        
        const size_t writable = capacity() - size;
        if (bytesToWrite > writable)
        {
            bytesToWrite = writable; // Buffer is full: truncate to fit.
        }

        for (size_t i = 0; i < bytesToWrite; ++i)
        {
            buffer[head] = data[i];
            head = (head + 1) % capacity();
        }
        size += bytesToWrite;
        return bytesToWrite;
    }

    // Consumer side: read up to `bytesToRead` bytes into `dest`. Returns the
    // number of bytes actually read (less than requested when empty).
    size_t read(uint8_t *dest, size_t bytesToRead)
    {
        if (dest == nullptr || bytesToRead == 0)
            return 0;

        std::lock_guard<std::mutex> lock(mtx);

        if (bytesToRead > size)
        {
            bytesToRead = size; // Buffer is low: read what is available.
        }

        for (size_t i = 0; i < bytesToRead; ++i)
        {
            dest[i] = buffer[tail];
            tail = (tail + 1) % capacity();
        }
        size -= bytesToRead;
        return bytesToRead;
    }

    size_t getAvailableBytes() const { return size; }
    size_t getCapacity() const { return capacity(); }

private:
    size_t capacity() const { return buffer.size(); }

    std::vector<uint8_t> buffer;
    size_t head;
    size_t tail;
    size_t size;
    mutable std::mutex mtx;
};