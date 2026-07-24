// scanbadblocks - check USB drives, SSDs and other disks by reading and optionally writing all blocks
//
// Copyright (c) 2025 Johannes Overmann
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <filesystem>
#include <cerrno>
#include <cstring>
#include <utility>
#include <functional>
#include <numeric>
#include <ranges>
#include <format>
#include <fstream>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>      // open()
#include <unistd.h>     // read(), write(), close()
#include <sys/stat.h>   // fstat()
#ifdef __linux__
#include <sys/ioctl.h>  // ioctl()
#include <linux/fs.h>   // BLKSSZGET
#endif
#include "CommandLineParser.hpp"
#include "MiscUtils.hpp"
#include "UnitTest.hpp"

static uint64_t verbose = 0; // --verbose
static volatile std::sig_atomic_t interruptRequested = 0;

extern "C" void handleInterrupt(int)
{
    interruptRequested = 1;
}

const double MB = 1024.0 * 1024.0;
const double GB = 1024.0 * 1024.0 * 1024.0;

class BlockChecker
{
public:
    BlockChecker(const std::string& filename_, const std::string& blockSizeStr, const std::string& strideSizeStr, const std::string& offsetStr, const std::string& sizeStr, const std::string& patternStr, const std::string& outfile_)
    {
        filename = filename_;
        outfile = outfile_;
        blockSize = ut1::strToU64(blockSizeStr);
        strideSize = strideSizeStr.empty() ? blockSize : ut1::strToU64(strideSizeStr);
        offsetBytes = ut1::strToU64(offsetStr);
        hasOverrideSize = !sizeStr.empty();
        overrideSizeBytes = hasOverrideSize ? ut1::strToU64(sizeStr) : 0;
        if (blockSize == 0)
        {
            throw std::runtime_error("Block size must be greater than zero!");
        }
        if (strideSize == 0)
        {
            throw std::runtime_error("Stride must be greater than zero!");
        }
        if (strideSize < blockSize)
        {
            throw std::runtime_error("Stride must be greater than or equal to block size!");
        }
        if (hasOverrideSize && overrideSizeBytes == 0)
        {
            throw std::runtime_error("Override size must be greater than zero!");
        }
        patterns = ut1::csvIntegersToVector<uint8_t>(patternStr, 16);
        detectedSizeBytes = ut1::getFileSize(filename);
        if (detectedSizeBytes == 0)
        {
            throw std::runtime_error("Cannot determine size!");
        }
        sizeBytes = detectedSizeBytes;
        if (hasOverrideSize)
        {
            if (overrideSizeBytes > detectedSizeBytes)
            {
                throw std::runtime_error(std::format("Override size {} is larger than detected size {}.", ut1::getPreciseSizeStr(overrideSizeBytes), ut1::getPreciseSizeStr(detectedSizeBytes)));
            }
            sizeBytes = overrideSizeBytes;
        }
        if (offsetBytes >= sizeBytes)
        {
            throw std::runtime_error("Offset must be smaller than device size!");
        }
        scanSizeBytes = sizeBytes - offsetBytes;
        numBlocks = (scanSizeBytes + strideSize - 1) / strideSize;
        blockStats.resize(numBlocks);
        if (numBlocks > 0)
        {
            totalAccessBytesOnePass = (numBlocks - 1) * blockSize + getAccessSize(numBlocks - 1);
        }
        std::string detectedSizeSuffix;
        if (hasOverrideSize)
        {
            detectedSizeSuffix = std::format(", detectedSize={}", ut1::getPreciseSizeStr(detectedSizeBytes));
        }
        std::cout << std::format("{}: Size={:.1f} GB ({} kBytes{}, numBlocks={}, blockSize={}, stride={}, offset={}, scanSize={}, size is a multiple of {})\n",
            filename_, sizeBytes / GB, getKBytes(sizeBytes), detectedSizeSuffix, numBlocks,
            ut1::getPreciseSizeStr(blockSize), ut1::getPreciseSizeStr(strideSize), ut1::getPreciseSizeStr(offsetBytes),
            getSizeStr(scanSizeBytes), ut1::getPreciseSizeStr(ut1::getLargestPowerOfTwoFactor(sizeBytes)));
    }

    void checkReadOnly()
    {
        numPasses = 1;
        readPassesRemaining = 1;
        writePassesRemaining = 0;
        readPass();
    }

    void checkWriteRead()
    {
        numPasses = patterns.size() * 2;
        readPassesRemaining = numPasses / 2;
        writePassesRemaining = numPasses / 2;
        for (size_t i = 0; i < patterns.size(); i++)
        {
            writePass(patterns[i]);
            readPass(patterns[i]);
        }
    }

    void checkNonDestructiveWrite()
    {
#ifndef O_DIRECT
        throw std::runtime_error("--non-destructive-write requires O_DIRECT support.");
#else
        ScopedInterruptHandler interruptHandler;
        numPasses = 1;
        readPassesRemaining = 1;
        writePassesRemaining = 1;
        blockStats.clear();
        blockStats.resize(numBlocks);
        lastProgressTime = ut1::getTimeSec();
        lastProgressReadBytes = totalRead.bytes;
        lastProgressWriteBytes = totalWrite.bytes;
        lastProgressReadTime = totalRead.time;
        lastProgressWriteTime = totalWrite.time;
        nonDestructiveProgress = true;

        DirectFile file(filename, O_RDWR | O_DIRECT, "direct non-destructive write");
        const int fd = file.get();

        const size_t alignment = getDirectIoAlignment(fd);
        DirectBuffer original(blockSize, alignment);
        DirectBuffer testData(blockSize, alignment);
        DirectBuffer verify(blockSize, alignment);

        for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++)
        {
            if (interruptRequested)
            {
                throw std::runtime_error("Interrupted.");
            }
            const size_t accessSize = getAccessSize(blockIndex);
            const size_t offset = getBlockOffset(blockIndex);
            validateDirectIoRequest(offset, accessSize, alignment);

            double elapsedRead = 0.0;
            double elapsedWrite = 0.0;
            bool ok = false;

            const bool originalReadOk = directRead(fd, original.data(), accessSize, offset, blockIndex, elapsedRead);
            if (originalReadOk)
            {
                if (!interruptRequested)
                {
                    fillPseudoRandom(testData.data(), accessSize, blockIndex);
                    ok = directWrite(fd, testData.data(), accessSize, offset, blockIndex, elapsedWrite);
                    if (ok && !interruptRequested)
                    {
                        ok = directRead(fd, verify.data(), accessSize, offset, blockIndex, elapsedRead);
                        if (ok && std::memcmp(verify.data(), testData.data(), accessSize) != 0)
                        {
                            std::cout << std::format("Data error: Non-destructive write verification failed (block {}).\n", blockIndex);
                            blockStats[blockIndex].errors++;
                            totalRead.errors++;
                            ok = false;
                        }
                    }
                    const bool restoreOk = directWrite(fd, original.data(), accessSize, offset, blockIndex, elapsedWrite);
                    if (!restoreOk)
                    {
                        throw std::runtime_error(std::format("Failed to restore original data for block {}.", blockIndex));
                    }
                }
            }

            blockStats[blockIndex].time += elapsedRead + elapsedWrite;
            if (originalReadOk)
            {
                blockStats[blockIndex].bytes += accessSize;
            }
            printProgress(blockIndex);
            if (interruptRequested)
            {
                throw std::runtime_error("Interrupted after restoring original block data.");
            }
        }

        printPassStats(/*read=*/false);
        passIndex++;
        nonDestructiveProgress = false;
#endif
    }

    void printResult()
    {
        std::cout << std::format("Transfer rates: read={:.1f}MB/s write={:.1f}MB/s\n", getReadBytesPerSecond() / MB, getWriteBytesPerSecond() / MB);
        if (totalRead.errors + totalWrite.errors > 0)
        {
            std::cout << std::format("ERROR: {} errors detected ({} read errors, {} write errors)\n", totalRead.errors + totalWrite.errors, totalRead.errors, totalWrite.errors);
        }
        else
        {
            std::cout << "OK: No errors detected.\n";
        }
    }

private:

    void readPass(std::optional<uint8_t> pattern = std::nullopt)
    {
        readPassesRemaining--;
        blockStats.clear();
        blockStats.resize(numBlocks);
        lastProgressTime = ut1::getTimeSec();
        lastProgressReadBytes = totalRead.bytes;
        lastProgressWriteBytes = totalWrite.bytes;
        lastProgressReadTime = totalRead.time;
        lastProgressWriteTime = totalWrite.time;

        int fd = ::open(filename.c_str(), O_RDONLY);
        if (fd < 0)
        {
            throw std::runtime_error(std::format("Error opening file '{}' for reading ({})", filename, strerror(errno)));
        }

        std::vector<uint8_t> buffer(blockSize);
        std::vector<uint8_t> expected(blockSize, pattern.value_or(0));

        for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++)
        {
            size_t accessSize = getAccessSize(blockIndex);
            double startTime = ut1::getTimeSec();
            ssize_t result = ::pread(fd, buffer.data(), accessSize, getBlockOffset(blockIndex));
            double elapsed = ut1::getTimeSec() - startTime;
            if (result < 0)
            {
                blockStats[blockIndex].errors++;
                totalRead.errors++;
            }
            else
            {
                if (pattern)
                {
                    initBlock(expected, blockIndex, *pattern);
                    for (size_t i = 0; i < accessSize; i++)
                    {
                        if (buffer[i] != expected[i])
                        {
                            std::cout << std::format("Data error: Expected {:#04x} and got {:#04x} (block {}).\n", expected[i], buffer[i], blockIndex);
                            blockStats[blockIndex].errors++;
                            totalRead.errors++;
                            break;
                        }
                    }
                }
                blockStats[blockIndex].time += elapsed;
                blockStats[blockIndex].bytes += accessSize;
                totalRead.time += elapsed;
                totalRead.bytes += accessSize;
            }
            printProgress(blockIndex);
        }

        close(fd);
        printPassStats(/*read=*/true);
        passIndex++;
    }

    void writePass(uint8_t pattern)
    {
        writePassesRemaining--;
        blockStats.clear();
        blockStats.resize(numBlocks);
        lastProgressTime = ut1::getTimeSec();
        lastProgressReadBytes = totalRead.bytes;
        lastProgressWriteBytes = totalWrite.bytes;
        lastProgressReadTime = totalRead.time;
        lastProgressWriteTime = totalWrite.time;

        // Open outfile.
        int  fd = ::open(filename.c_str(), O_WRONLY, 0666);
        if (fd < 0)
        {
            throw std::runtime_error(std::format("Error opening file '{}' for writing ({})", filename, strerror(errno)));
        }

        std::vector<uint8_t> buffer(blockSize, pattern);

        for (size_t blockIndex = 0; blockIndex < numBlocks; blockIndex++)
        {
            initBlock(buffer, blockIndex, pattern);
            size_t accessSize = getAccessSize(blockIndex);

            double startTime = ut1::getTimeSec();
            ssize_t result = pwrite(fd, buffer.data(), accessSize, getBlockOffset(blockIndex));
            double elapsed = ut1::getTimeSec() - startTime;
            if (result < 0)
            {
                blockStats[blockIndex].errors++;
                totalWrite.errors++;
            }
            else
            {
                blockStats[blockIndex].time += elapsed;
                blockStats[blockIndex].bytes += accessSize;
                totalWrite.time += elapsed;
                totalWrite.bytes += accessSize;
            }
            printProgress(blockIndex);
        }

        close(fd);
        printPassStats(/*read=*/false);
        passIndex++;
    }

    void printProgress(size_t blockIndex)
    {
        double now = ut1::getTimeSec();
        double elapsed = now - lastProgressTime;
        bool linePerBlock = verbose > 0;
        if (!linePerBlock && elapsed < 0.5)
        {
            return;
        }
        double totalRangeBytesOnePass = double(scanSizeBytes);
        double totalBytesOnePass = double(totalAccessBytesOnePass);
        double totalReadBytes = totalBytesOnePass;
        double totalWriteBytes = 0.0;
        std::string prefix;
        if (nonDestructiveProgress)
        {
            prefix = std::format("non-destructive write pass {}/{}: ", passIndex + 1, numPasses);
            totalReadBytes = 2 * totalBytesOnePass;
            totalWriteBytes = 2 * totalBytesOnePass;
        }
        if (numPasses > 1)
        {
            prefix = std::format("{} pass {}/{} (pat {:02x}): ", (passIndex & 1 ? "read" : "write"), passIndex + 1, numPasses, patterns[passIndex / 2]);
            totalReadBytes = numPasses / 2 * totalBytesOnePass;
            totalWriteBytes = numPasses / 2 * totalBytesOnePass;
        }

        double rangeBytes = std::min(double(blockIndex + 1) * strideSize, double(scanSizeBytes));
        double currentReadTime = totalRead.time - lastProgressReadTime;
        double currentWriteTime = totalWrite.time - lastProgressWriteTime;
        double currentReadBytesPerSecond = currentReadTime > 0.0 ? (totalRead.bytes - lastProgressReadBytes) / currentReadTime : 0.0;
        double currentWriteBytesPerSecond = currentWriteTime > 0.0 ? (totalWrite.bytes - lastProgressWriteBytes) / currentWriteTime : 0.0;
        double percent = (passIndex * totalRangeBytesOnePass + rangeBytes) / (numPasses * totalRangeBytesOnePass) * 100.0;
        double remainingSec = 0.0;
        if (getReadBytesPerSecond() > 0.0)
        {
            remainingSec += (totalReadBytes - totalRead.bytes) / getReadBytesPerSecond();
        }
        else if (getWriteBytesPerSecond() > 0.0)
        {
            // Approximate read speed with write speed for the very first write pass.
            remainingSec += (totalReadBytes - totalRead.bytes) / getWriteBytesPerSecond();
        }
        if (getWriteBytesPerSecond() > 0.0)
        {
            remainingSec += (totalWriteBytes - totalWrite.bytes) / getWriteBytesPerSecond();
        }
        std::cout << prefix << std::format("{:6d}/{:6d} {:.1f}/{:.1f}MB {:4.1f}% remaining={} read={:.1f}MB/s(avg={:.1f}) write={:.1f}MB/s(avg={:.1f}){}",
            blockIndex, numBlocks, rangeBytes / MB, totalRangeBytesOnePass / MB,
            percent, ut1::secondsToString(remainingSec),
            currentReadBytesPerSecond / MB, getReadBytesPerSecond() / MB,
            currentWriteBytesPerSecond / MB, getWriteBytesPerSecond() / MB,
            linePerBlock ? "\n" : "   \r") << std::flush;
        lastProgressTime = now;
        lastProgressReadBytes = totalRead.bytes;
        lastProgressWriteBytes = totalWrite.bytes;
        lastProgressReadTime = totalRead.time;
        lastProgressWriteTime = totalWrite.time;
    }

    double getReadBytesPerSecond()
    {
        if ((totalRead.time > 0.0) && (totalRead.bytes > 0.0))
        {
            return totalRead.bytes / totalRead.time;
        }
        return 0.0;
    }

    double getWriteBytesPerSecond()
    {
        if ((totalWrite.time > 0.0) && (totalWrite.bytes > 0.0))
        {
            return totalWrite.bytes / totalWrite.time;
        }
        return 0.0;
    }

    void printPassStats(bool read)
    {
        std::string readWrite = nonDestructiveProgress ? "non-destructive-write" : (read ? "read" : "write");
        // Write outfile.
        if (!outfile.empty())
        {
            std::string outfilename = std::format("{}_{}{}_{}.txt", outfile, readWrite, passIndex, sizeBytes);
            std::ofstream os(outfilename);
            if (!os)
            {
                throw std::runtime_error(std::format("Error while opening file '{}' for writing!", outfilename));
            }
            os << std::dec << std::scientific;
            for (size_t i = 0; i < numBlocks; i++)
            {
                os << i << "," << blockStats[i].time << "," << blockStats[i].errors << "\n";
            }
        }

        // Collect stats.
        std::sort(blockStats.begin(), blockStats.end(), [](const BlockStats& a, const BlockStats& b) { return a.getRateMB() < b.getRateMB(); });
        double min = blockStats[0].getRateMB();
        double max = blockStats[numBlocks - 1].getRateMB();
        double med = blockStats[numBlocks / 2].getRateMB();
        double totalTime = std::accumulate(blockStats.begin(), blockStats.end(), 0.0, [](auto acc, const auto& r) { return acc + r.time; });
        size_t totalBytes = std::accumulate(blockStats.begin(), blockStats.end(), size_t(0), [](auto acc, const auto& r) { return acc + r.bytes; });
        size_t errors = std::accumulate(blockStats.begin(), blockStats.end(), 0, [](auto acc, const auto& r) { return acc + r.errors; });
        double avg = 0.0;
        if (totalTime > 0.0)
        {
            avg = totalBytes / totalTime / MB;
        }
        std::cout << std::format("pass {}/{} ({}): {} errors (time={} min={:.1f}MB/s avg={:.1f}MB/s med={:.1f}MB/s max={:.1f}MB/s)                        \n",
            passIndex + 1, numPasses, readWrite, errors, ut1::secondsToString(totalTime), min, avg, med, max);
        std::vector<double> percentiles({50, 20, 10, 5});
        for (double percent: percentiles)
        {
            size_t num = 0;
            for (; num < numBlocks && blockStats[num].getRateMB() < med * percent / 100.0; num++);
            if (num)
            {
                std::cout << std::format("Warning: Number of blocks slower than {:.0f}% of median: {}\n", percent, num);
            }
        }
    }

    void initBlock(std::vector<uint8_t>& buffer, size_t blockIndex, uint8_t pattern)
    {
        buffer[0] = pattern ^ (blockIndex & 0xff);
        buffer[1] = pattern ^ ((blockIndex >> 8) & 0xff);
        buffer[2] = pattern ^ ((blockIndex >> 16) & 0xff);
        buffer[3] = pattern ^ ((blockIndex >> 24) & 0xff);
        buffer[4] = pattern ^ ((blockIndex >> 32) & 0xff);
        buffer[5] = pattern ^ ((blockIndex >> 40) & 0xff);
        buffer[6] = pattern ^ ((blockIndex >> 48) & 0xff);
        buffer[7] = pattern ^ ((blockIndex >> 56) & 0xff);
    }

    class ScopedInterruptHandler
    {
    public:
        ScopedInterruptHandler()
        {
            interruptRequested = 0;
            struct sigaction action {};
            action.sa_handler = handleInterrupt;
            sigemptyset(&action.sa_mask);
            action.sa_flags = SA_RESTART;
            if (::sigaction(SIGINT, &action, &previousAction) != 0)
            {
                throw std::runtime_error(std::format("Error installing Ctrl-C handler ({})", strerror(errno)));
            }
        }

        ~ScopedInterruptHandler()
        {
            ::sigaction(SIGINT, &previousAction, nullptr);
        }

        ScopedInterruptHandler(const ScopedInterruptHandler&) = delete;
        ScopedInterruptHandler& operator=(const ScopedInterruptHandler&) = delete;

    private:
        struct sigaction previousAction {};
    };

    class DirectBuffer
    {
    public:
        DirectBuffer(size_t size, size_t alignment)
        : bufferSize(size)
        {
            if (::posix_memalign(&buffer, alignment, size) != 0)
            {
                throw std::runtime_error("Error allocating aligned direct I/O buffer.");
            }
        }

        ~DirectBuffer()
        {
            std::free(buffer);
        }

        DirectBuffer(const DirectBuffer&) = delete;
        DirectBuffer& operator=(const DirectBuffer&) = delete;

        uint8_t* data() noexcept { return static_cast<uint8_t*>(buffer); }
        size_t size() const noexcept { return bufferSize; }

    private:
        void* buffer{};
        size_t bufferSize{};
    };

    class DirectFile
    {
    public:
        DirectFile(const std::string& filename, int flags, const std::string& operation)
        {
            fd = ::open(filename.c_str(), flags);
            if (fd < 0)
            {
                throw std::runtime_error(std::format("Error opening file '{}' for {} ({})", filename, operation, strerror(errno)));
            }
        }

        ~DirectFile()
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }

        DirectFile(const DirectFile&) = delete;
        DirectFile& operator=(const DirectFile&) = delete;

        int get() const noexcept { return fd; }

    private:
        int fd{-1};
    };

    size_t getDirectIoAlignment(int fd) const
    {
#ifdef __linux__
        int logicalBlockSize = 0;
        if (::ioctl(fd, BLKSSZGET, &logicalBlockSize) == 0 && logicalBlockSize > 0)
        {
            return static_cast<size_t>(logicalBlockSize);
        }
#endif
        struct stat st {};
        if (::fstat(fd, &st) == 0 && st.st_blksize > 0)
        {
            return static_cast<size_t>(st.st_blksize);
        }
        return 4096;
    }

    void validateDirectIoRequest(size_t offset, size_t accessSize, size_t alignment) const
    {
        if ((offset % alignment) != 0 || (accessSize % alignment) != 0)
        {
            throw std::runtime_error(std::format("O_DIRECT requires offset and access size to be multiples of {} bytes. Try --block-size and --offset values aligned to the device logical block size.", alignment));
        }
    }

    bool directRead(int fd, uint8_t* buffer, size_t accessSize, size_t offset, size_t blockIndex, double& elapsed)
    {
        const double startTime = ut1::getTimeSec();
        const bool ok = exactPread(fd, buffer, accessSize, offset);
        const double duration = ut1::getTimeSec() - startTime;
        elapsed += duration;
        if (!ok)
        {
            std::cout << std::format("Read error: block {} ({})\n", blockIndex, strerror(errno));
            blockStats[blockIndex].errors++;
            totalRead.errors++;
        }
        else
        {
            totalRead.bytes += accessSize;
            totalRead.time += duration;
        }
        return ok;
    }

    bool directWrite(int fd, const uint8_t* buffer, size_t accessSize, size_t offset, size_t blockIndex, double& elapsed)
    {
        const double startTime = ut1::getTimeSec();
        const bool ok = exactPwrite(fd, buffer, accessSize, offset);
        const double duration = ut1::getTimeSec() - startTime;
        elapsed += duration;
        if (!ok)
        {
            std::cout << std::format("Write error: block {} ({})\n", blockIndex, strerror(errno));
            blockStats[blockIndex].errors++;
            totalWrite.errors++;
        }
        else
        {
            totalWrite.bytes += accessSize;
            totalWrite.time += duration;
        }
        return ok;
    }

    bool exactPread(int fd, uint8_t* buffer, size_t size, size_t offset)
    {
        size_t done = 0;
        while (done < size)
        {
            ssize_t result = ::pread(fd, buffer + done, size - done, offset + done);
            if (result <= 0)
            {
                if (result < 0 && errno == EINTR)
                {
                    continue;
                }
                if (result == 0)
                {
                    errno = EIO;
                }
                return false;
            }
            done += static_cast<size_t>(result);
        }
        return true;
    }

    bool exactPwrite(int fd, const uint8_t* buffer, size_t size, size_t offset)
    {
        size_t done = 0;
        while (done < size)
        {
            ssize_t result = ::pwrite(fd, buffer + done, size - done, offset + done);
            if (result <= 0)
            {
                if (result < 0 && errno == EINTR)
                {
                    continue;
                }
                if (result == 0)
                {
                    errno = EIO;
                }
                return false;
            }
            done += static_cast<size_t>(result);
        }
        return true;
    }

    uint64_t pseudoRandomNext(uint64_t& state) const
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    void fillPseudoRandom(uint8_t* buffer, size_t size, size_t blockIndex) const
    {
        uint64_t state = 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(blockIndex) * 0xbf58476d1ce4e5b9ULL);
        for (size_t i = 0; i < size; i++)
        {
            if ((i & 7U) == 0)
            {
                pseudoRandomNext(state);
            }
            buffer[i] = static_cast<uint8_t>(state >> ((i & 7U) * 8U));
        }
    }

    std::string getSizeStr(size_t bytes) const
    {
        return ut1::getApproxSizeStr(static_cast<uint64_t>(bytes), 1, true, true);
    }

    size_t getKBytes(size_t bytes) const
    {
        return bytes / 1024;
    }

    size_t getBlockOffset(size_t blockIndex) const
    {
        return offsetBytes + blockIndex * strideSize;
    }

    size_t getAccessSize(size_t blockIndex) const
    {
        size_t offset = getBlockOffset(blockIndex);
        return std::min(blockSize, sizeBytes - offset);
    }

    // Input:
    std::string filename;
    std::string outfile;
    size_t blockSize{};
    size_t strideSize{};
    size_t offsetBytes{};
    bool hasOverrideSize{};
    size_t overrideSizeBytes{};
    size_t detectedSizeBytes{};
    size_t sizeBytes{};
    size_t scanSizeBytes{};
    size_t numBlocks{};
    size_t totalAccessBytesOnePass{};
    std::vector<uint8_t> patterns;

    // Measurements:
    struct BlockStats
    {
        double time{};
        size_t errors{};
        size_t bytes{};
        double getRateMB() const { if (time > 0.0) return bytes / time / MB; else return 0.0; }
    };
    std::vector<BlockStats> blockStats;
    BlockStats totalWrite;
    BlockStats totalRead;

    // State:
    double lastProgressTime{};
    size_t lastProgressReadBytes{};
    size_t lastProgressWriteBytes{};
    double lastProgressReadTime{};
    double lastProgressWriteTime{};
    size_t passIndex{};
    size_t readPassesRemaining{};
    size_t writePassesRemaining{};
    size_t numPasses{};
    bool nonDestructiveProgress{};
};


void dropLinuxCaches()
{
#ifdef __linux__
    ::sync();
    std::ofstream os("/proc/sys/vm/drop_caches");
    if (!os)
    {
        throw std::runtime_error(std::format("Error opening /proc/sys/vm/drop_caches for writing ({}). Try running as root.", strerror(errno)));
    }
    os << "3\n";
    if (!os)
    {
        throw std::runtime_error("Error writing to /proc/sys/vm/drop_caches. Try running as root.");
    }
    std::cout << "Dropped Linux page cache, dentries and inodes.\n";
#else
    throw std::runtime_error("--drop-caches is only supported on Linux.");
#endif
}


/// Main.
int main(int argc, char* argv[])
{
    // Run unit tests and exit if enabled at compile time.
    UNIT_TEST_RUN();

    try
    {
        // Command line options.
        ut1::CommandLineParser cl("scanbadblocks", "Check block device by reading all blocks and optionally writing them.\n"
                                  "\n"
                                  "Usage: $programName [OPTIONS] BLOCK_DEVICE\n"
                                  "\n",
                                  "$programName version $version ($compileDate) *** Copyright (c) 2025-2026 Johannes Overmann *** https://github.com/jovermann/scanbadblocks",
                                  "1.0.4");

        cl.addHeader("\nOptions:\n");
        cl.addOption('b', "block-size", "Granularity of reads/writes in bytes.", "BLOCKSIZE", "4M");
        cl.addOption('s', "stride", "Distance between read/write offsets. The default tracks --block-size. Use values larger than --block-size to sample the full disk range, e.g. --block-size=1M --stride=1G.", "STRIDE");
        cl.addOption(0, "offset", "Initial offset before applying --stride. Use this to scan another interleaved part of the disk, e.g. --block-size=1M --stride=1G --offset=512M.", "OFFSET", "0");
        cl.addOption('S', "size", "Override detected disk size. Useful to limit tests to a smaller initial portion of a disk, e.g. --size=100G.", "SIZE");
        cl.addOption('w', "overwrite", "Overwrite device with known pattern and then read it back. This immediately destroys the contents of the disk, erases the disk and deletes all files on the disk. Specify twice to override interactive safety prompt. The default is just to read the disk.");
        cl.addOption('n', "non-destructive-write", "Read each block, write deterministic pseudorandom data with O_DIRECT, verify it, then restore the original block contents. Similar to badblocks -n.");
        cl.addOption('p', "pattern", "Comma separated list of one or more hexadecimal byte values for --overwrite. Each byte will result in one write pass and one read pass on the disk. Useful patterns to clear the disk 4 times are 55,aa,00,ff. The default is 00 resulting in one write pass and one read pass.", "PATTERN", "00");
        cl.addOption('o', "outfile", "Write timing data to CSV files of the format PREFIX_PASS_DISKSIZE.txt. ", "PREFIX", "scanbadblocks");
        cl.addOption('d', "drop-caches", "Linux only: sync and drop page cache, dentries and inodes before starting. May be used without BLOCK_DEVICE to only drop caches.");
        cl.addOption('v', "verbose", "Increase verbosity. Specify multiple times to be more verbose.");

        // Parse command line options.
        cl.parse(argc, argv);
        if (cl("drop-caches"))
        {
            dropLinuxCaches();
        }
        if (cl.getArgs().empty())
        {
            if (cl("drop-caches"))
            {
                return 0;
            }
            cl.error("Missing argument: BLOCK_DEVICE.\n");
        }
        if (cl.getArgs().size() != 1)
        {
            cl.error("Too many arguments.\n");
        }
        std::string filename = cl.getArgs()[0];
        if (!ut1::fsExists(filename))
        {
            cl.error(std::format("File '{}' does not exist!\n", filename));
        }
        verbose = cl.getUInt("verbose");
        if (cl("overwrite") && cl("non-destructive-write"))
        {
            cl.error("--overwrite and --non-destructive-write are mutually exclusive.\n");
        }

        BlockChecker blockChecker(filename, cl.getStr("block-size"), cl.getStr("stride"), cl.getStr("offset"), cl.getStr("size"), cl.getStr("pattern"), cl.getStr("outfile"));

        if (cl("overwrite"))
        {
            // Write/read mode.
            if (cl.getCount("overwrite") < 2)
            {
                std::cout << "Please enter OVERWRITE and press enter to confirm deleting all data on '" << filename << ":\n";
                std::string line;
                std::getline(std::cin, line);
                if (line != "OVERWRITE")
                {
                    std::cout << "Not confirmed. Exiting.\n";
                    std::exit(0);
                }
            }
            blockChecker.checkWriteRead();
        }
        else if (cl("non-destructive-write"))
        {
            blockChecker.checkNonDestructiveWrite();
        }
        else
        {
            // Read-only mode.
            blockChecker.checkReadOnly();
        }

        blockChecker.printResult();
    }
    catch (const std::exception &e)
    {
        ut1::CommandLineParser::reportErrorAndExit(e.what());
    }

    return 0;
}
