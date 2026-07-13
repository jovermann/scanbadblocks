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
#include <fcntl.h>      // open()
#include <unistd.h>     // read(), write(), close()
#include "CommandLineParser.hpp"
#include "MiscUtils.hpp"
#include "UnitTest.hpp"

static uint64_t verbose = 0; // --verbose

const double MB = 1024.0 * 1024.0;
const double GB = 1024.0 * 1024.0 * 1024.0;

class BlockChecker
{
public:
    BlockChecker(const std::string& filename_, const std::string& blockSizeStr, const std::string& strideSizeStr, const std::string& offsetStr, const std::string& patternStr, const std::string& outfile_)
    {
        filename = filename_;
        outfile = outfile_;
        blockSize = ut1::strToU64(blockSizeStr);
        strideSize = strideSizeStr.empty() ? blockSize : ut1::strToU64(strideSizeStr);
        offsetBytes = ut1::strToU64(offsetStr);
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
        patterns = ut1::csvIntegersToVector<uint8_t>(patternStr, 16);
        sizeBytes = ut1::getFileSize(filename);
        if (sizeBytes == 0)
        {
            throw std::runtime_error("Cannot determine size!");
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
        std::cout << std::format("{}: Size={:.1f} GB ({}, numBlocks={}, blockSize={}, stride={}, offset={}, scanSize={}, size is a multiple of {})\n",
            filename_, sizeBytes / GB, ut1::getApproxSizeStr(sizeBytes, 1), numBlocks,
            ut1::getPreciseSizeStr(blockSize), ut1::getPreciseSizeStr(strideSize), ut1::getPreciseSizeStr(offsetBytes),
            ut1::getPreciseSizeStr(scanSizeBytes), ut1::getPreciseSizeStr(ut1::getLargestPowerOfTwoFactor(sizeBytes)));
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
        lastProgressBytes = 0.0;
        currentPassBytes = 0;

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
                currentPassBytes += accessSize;
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
        lastProgressBytes = 0.0;
        currentPassBytes = 0;

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
                currentPassBytes += accessSize;
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
        if (numPasses > 1)
        {
            prefix = std::format("{} pass {}/{} (pat {:02x}): ", (passIndex & 1 ? "read" : "write"), passIndex + 1, numPasses, patterns[passIndex / 2]);
            totalReadBytes = numPasses / 2 * totalBytesOnePass;
            totalWriteBytes = numPasses / 2 * totalBytesOnePass;
        }

        double rangeBytes = std::min(double(blockIndex + 1) * strideSize, double(scanSizeBytes));
        double currentBytesPerSecond = elapsed > 0.0 ? (currentPassBytes - lastProgressBytes) / elapsed : 0.0;
        const bool currentPassIsRead = (numPasses == 1) || (passIndex & 1);
        double currentReadBytesPerSecond = currentPassIsRead ? currentBytesPerSecond : 0.0;
        double currentWriteBytesPerSecond = currentPassIsRead ? 0.0 : currentBytesPerSecond;
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
        lastProgressBytes = currentPassBytes;
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
        std::string readWrite = read ? "read" : "write";
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
    double lastProgressBytes{};
    size_t currentPassBytes{};
    size_t passIndex{};
    size_t readPassesRemaining{};
    size_t writePassesRemaining{};
    size_t numPasses{};
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
        cl.addOption('w', "overwrite", "Overwrite device with known pattern and then read it back. This immediately destroys the contents of the disk, erases the disk and deletes all files on the disk. Specify twice to override interactive safety prompt. The default is just to read the disk.");
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

        BlockChecker blockChecker(filename, cl.getStr("block-size"), cl.getStr("stride"), cl.getStr("offset"), cl.getStr("pattern"), cl.getStr("outfile"));

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
