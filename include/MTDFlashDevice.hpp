#ifndef MTD_FLASH_DEV_HPP
#define MTD_FLASH_DEV_HPP

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

class MTDFlashDevice {
private:
    int fd;
    struct mtd_info_user mtdInfo;
    std::string devicePath;

public:
    MTDFlashDevice(const std::string& path) : fd(-1), devicePath(path) {}

    ~MTDFlashDevice() {
        close();
    }

    bool open() {
        fd = ::open(devicePath.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "Error opening device " << devicePath << ": " << strerror(errno) << std::endl;
            return false;
        }

        // MTDデバイス情報の取得
        if (ioctl(fd, MEMGETINFO, &mtdInfo) < 0) {
            std::cerr << "Error getting MTD information: " << strerror(errno) << std::endl;
            ::close(fd);
            fd = -1;
            return false;
        }
#ifdef DEBUG
        std::cout << "MTD Device Information:" << std::endl;
        std::cout << "  Type: " << mtdInfo.type << std::endl;
        std::cout << "  Size: " << mtdInfo.size << " bytes" << std::endl;
        std::cout << "  Erase size: " << mtdInfo.erasesize << " bytes" << std::endl;
        std::cout << "  Write size: " << mtdInfo.writesize << " bytes" << std::endl;
        std::cout << "  OOB size: " << mtdInfo.oobsize << " bytes" << std::endl;
#endif
        return true;
    }

    void close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    bool isOpen() const {
        return fd >= 0;
    }

    bool read(uint32_t offset, uint8_t* buffer, size_t length) {
        if (!isOpen()) {
            std::cerr << "Device not open" << std::endl;
            return false;
        }

        if (lseek(fd, offset, SEEK_SET) != static_cast<off_t>(offset)) {
            std::cerr << "Error read seeking to offset " << offset << ": " << strerror(errno) << std::endl;
            return false;
        }

        ssize_t bytesRead = ::read(fd, buffer, length);
        if (bytesRead < 0) {
            std::cerr << "Error reading from device: " << strerror(errno) << std::endl;
            return false;
        } else if (static_cast<size_t>(bytesRead) != length) {
            std::cerr << "Short read: " << bytesRead << " of " << length << " bytes" << std::endl;
            return false;
        }

        return true;
    }

    bool write(uint32_t offset, const uint8_t* buffer, size_t length) {
        if (!isOpen()) {
            std::cerr << "Device not open" << std::endl;
            return false;
        }

        // オフセットがイレースブロックアラインメントになっていることを確認
#if 0
        if (offset % mtdInfo.erasesize != 0) {
            std::cerr << "Warning: Write offset is not aligned to erase block size" << std::endl;
        }
#endif
        if (lseek(fd, offset, SEEK_SET) != static_cast<off_t>(offset)) {
            std::cerr << "Error write seeking to offset " << offset << ": " << strerror(errno) << std::endl;
            return false;
        }

        ssize_t bytesWritten = ::write(fd, buffer, length);
        if (bytesWritten < 0) {
            std::cerr << "Error writing to device: " << strerror(errno) << std::endl;
            return false;
        } else if (static_cast<size_t>(bytesWritten) != length) {
            std::cerr << "Short write: " << bytesWritten << " of " << length << " bytes" << std::endl;
            return false;
        }

        return true;
    }

    bool erase(uint32_t offset, uint32_t length) {
        if (!isOpen()) {
            std::cerr << "Device not open" << std::endl;
            return false;
        }

        // イレースブロックアラインメントのチェック
        if (offset % mtdInfo.erasesize != 0) {
            std::cerr << "Error: Erase offset must be aligned to erase block size" << std::endl;
            return false;
        }
        if (length % mtdInfo.erasesize != 0) {
            std::cerr << "Error: Erase length must be a multiple of erase block size" << std::endl;
            return false;
        }

        struct erase_info_user eraseInfo;
        eraseInfo.start = offset;
        eraseInfo.length = length;

        if (ioctl(fd, MEMERASE, &eraseInfo) < 0) {
            std::cerr << "Error erasing flash: " << strerror(errno) << std::endl;
            return false;
        }

        return true;
    }

    const mtd_info_user& getInfo() const {
        return mtdInfo;
    }
};

#endif //MTD_FLASH_DEV_HPP