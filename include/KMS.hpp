#ifndef KMS_HPP
#define KMS_HPP

#include <stdint.h>

// KuvixOS içerisindeki desteklenen dosya sistemi türleri
enum class FileSystemType {
    KuvixFS,
    FAT12,
    FAT16,
    EXT2,
    Unknown
};

// Her bir mount (bağlantı) noktasını temsil eden yapı
struct MountPoint {
    char path[128];             // Örn: "/mnt/disk1" veya "/"
    uint8_t drive_id;           // Sürücü numarası (Örn: ATA Master için 0)
    FileSystemType fs_type;     // Dosya sistemi türü
    void* fs_driver_instance;   // İlgili sürücü nesnesinin adresi (VFS entegrasyonu için)
    bool is_active;             // Bu slot dolu mu/aktif mi?
};

class KuvixFSMountSystem {
private:
    static const int MAX_MOUNT_POINTS = 16;
    MountPoint mount_list[MAX_MOUNT_POINTS];

    // String işlemleri için yardımcı iç fonksiyonlar (Kernel ortamında standart kütüphane olmayacağı için)
    int StringLength(const char* str);
    bool StringCompare(const char* str1, const char* str2);
    void StringCopy(char* dest, const char* src, int max_len);

public:
    KuvixFSMountSystem();
    ~KuvixFSMountSystem();

    // Yeni bir diski belirli bir yola bağlar
    bool Mount(const char* target_path, uint8_t drive_id, FileSystemType type);

    // Bağlı olan diskin bağlantısını keser
    bool Unmount(const char* target_path);

    // Verilen dosya yoluna göre hangi diskte olduğunu bulur (Longest Prefix Match)
    MountPoint* ResolvePath(const char* file_path);

    // Mevcut mount tablosunu terminale yazdırmak için (Simülasyon ve test amaçlı)
    void DumpMountTable();
};

#endif // KMS_HPP