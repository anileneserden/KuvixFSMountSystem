#include "../include/KMS.hpp"
#include <iostream>

int KuvixFSMountSystem::StringLength(const char* str) {
    if (!str) return 0;
    int len = 0;
    while (str[len] != '\0') len++;
    return len;
}

bool KuvixFSMountSystem::StringCompare(const char* str1, const char* str2) {
    if (!str1 || !str2) return false;
    int i = 0;
    while (str1[i] != '\0' && str2[i] != '\0') {
        if (str1[i] != str2[i]) return false;
        i++;
    }
    return (str1[i] == '\0' && str2[i] == '\0');
}

void KuvixFSMountSystem::StringCopy(char* dest, const char* src, int max_len) {
    if (!dest || !src || max_len <= 0) return;
    int i = 0;
    while (src[i] != '\0' && i < (max_len - 1)) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

KuvixFSMountSystem::KuvixFSMountSystem() {
    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        mount_list[i].is_active = false;
        mount_list[i].path[0] = '\0';
        mount_list[i].drive_id = 0;
        mount_list[i].fs_driver_instance = nullptr;
    }
}

KuvixFSMountSystem::~KuvixFSMountSystem() {}

bool KuvixFSMountSystem::Mount(const char* target_path, uint8_t drive_id, FileSystemType type) {
    if (!target_path) return false;

    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        if (mount_list[i].is_active && StringCompare(mount_list[i].path, target_path)) {
            return false;
        }
    }

    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        if (!mount_list[i].is_active) {
            StringCopy(mount_list[i].path, target_path, 128);
            mount_list[i].drive_id = drive_id;
            mount_list[i].fs_type = type;
            mount_list[i].fs_driver_instance = nullptr;
            mount_list[i].is_active = true;
            return true;
        }
    }
    return false;
}

bool KuvixFSMountSystem::Unmount(const char* target_path) {
    if (!target_path) return false;

    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        if (mount_list[i].is_active && StringCompare(mount_list[i].path, target_path)) {
            mount_list[i].is_active = false;
            mount_list[i].path[0] = '\0';
            return true;
        }
    }
    return false;
}

MountPoint* KuvixFSMountSystem::ResolvePath(const char* file_path) {
    if (!file_path) return nullptr;

    MountPoint* best_match = nullptr;
    int max_match_len = -1;

    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        if (!mount_list[i].is_active) continue;

        const char* m_path = mount_list[i].path;
        int m_len = StringLength(m_path);

        if (m_len == 1 && m_path[0] == '/') {
            if (max_match_len < 1) {
                best_match = &mount_list[i];
                max_match_len = 1;
            }
            continue;
        }

        bool match = true;
        for (int j = 0; j < m_len; j++) {
            if (file_path[j] != m_path[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            if (file_path[m_len] == '\0' || file_path[m_len] == '/') {
                if (m_len > max_match_len) {
                    max_match_len = m_len;
                    best_match = &mount_list[i];
                }
            }
        }
    }
    return best_match;
}

void KuvixFSMountSystem::DumpMountTable() {
    std::cout << "\n--- CURRENT MOUNT TABLE ---" << std::endl;
    std::cout << "Slot\tPath\t\tDrive ID\tFS Type" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;
    for (int i = 0; i < MAX_MOUNT_POINTS; i++) {
        if (mount_list[i].is_active) {
            std::cout << i << "\t" << mount_list[i].path << "\t\t" 
                      << (int)mount_list[i].drive_id << "\t\t";
            switch(mount_list[i].fs_type) {
                case FileSystemType::KuvixFS: std::cout << "KuvixFS"; break;
                case FileSystemType::FAT12:   std::cout << "FAT12"; break;
                case FileSystemType::FAT16:   std::cout << "FAT16"; break;
                case FileSystemType::EXT2:    std::cout << "EXT2"; break;
                default:                      std::cout << "Unknown"; break;
            }
            std::cout << std::endl;
        }
    }
    std::cout << "------------------------------------------------\n" << std::endl;
}