#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib> // system() fonksiyonu için
#include "include/KMS.hpp"

#define KVX_MAGIC     "KVXFS1"
#define KVX_MAX_FILES 256
#define KVX_DIR_SIZE  0xFFFFFFFFu

typedef struct {
    char     path[64];
    uint32_t start_lba;
    uint32_t size;      
    uint8_t  used;
    uint8_t  _pad[3];
} __attribute__((packed)) kvx_ent_t;

typedef struct {
    char     magic[8];
    uint32_t file_count;
    uint32_t next_free_lba;
    kvx_ent_t ent[KVX_MAX_FILES];
} __attribute__((packed)) kvx_meta_t;

KuvixFSMountSystem kms;
std::string g_img_path = "";
kvx_meta_t g_meta;

bool load_meta() {
    FILE* f = fopen(g_img_path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 2048 * 512, SEEK_SET);
    fread(&g_meta, sizeof(kvx_meta_t), 1, f);
    fclose(f);
    return (std::strncmp(g_meta.magic, KVX_MAGIC, 6) == 0);
}

void save_meta() {
    FILE* f = fopen(g_img_path.c_str(), "r+b");
    if (!f) return;
    fseek(f, 2048 * 512, SEEK_SET);
    fwrite(&g_meta, sizeof(kvx_meta_t), 1, f);
    fclose(f);
}

// 🔹 FUSE Çağrıları (Daha önce yazdığımız mantık)
static int kms_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;
    MountPoint* mp = kms.ResolvePath(path);
    if (!mp) return -ENOENT;

    filler(buf, ".", NULL, 0, (fuse_fill_dir_flags)0);
    filler(buf, "..", NULL, 0, (fuse_fill_dir_flags)0);

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used) {
            if (std::strcmp(g_meta.ent[i].path, path) == 0) continue;
            const char* name = g_meta.ent[i].path;
            if (path[1] != '\0') name += std::strlen(path);
            if (name[0] == '/') name++;
            
            if (std::strchr(name, '/') == nullptr && std::strlen(name) > 0) {
                filler(buf, name, NULL, 0, (fuse_fill_dir_flags)0);
            }
        }
    }
    return 0;
}

static int kms_fuse_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void) fi;
    std::memset(stbuf, 0, sizeof(struct stat));
    if (std::strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            if (g_meta.ent[i].size == KVX_DIR_SIZE) {
                stbuf->st_mode = S_IFDIR | 0755;
                stbuf->st_nlink = 2;
            } else {
                stbuf->st_mode = S_IFREG | 0644;
                stbuf->st_nlink = 1;
                stbuf->st_size = g_meta.ent[i].size;
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    (void) mode; (void) fi;
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (!g_meta.ent[i].used) {
            std::strncpy(g_meta.ent[i].path, path, 63);
            g_meta.ent[i].size = 0;
            g_meta.ent[i].start_lba = g_meta.next_free_lba;
            g_meta.ent[i].used = 1;
            g_meta.file_count++;
            save_meta();
            return 0;
        }
    }
    return -ENOSPC;
}

static int kms_fuse_unlink(const char* path) {
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            g_meta.ent[i].used = 0;
            g_meta.file_count--;
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_read(const char* path, char* buf, size_t size, off_t offset,
                         struct fuse_file_info* fi) {
    (void) fi;
    
    // 1. Dosyayı meta tabloda ara
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            if (offset >= g_meta.ent[i].size) return 0;
            if (offset + size > g_meta.ent[i].size) {
                size = g_meta.ent[i].size - offset;
            }

            // 2. İmaj dosyasından veriyi oku
            FILE* f = fopen(g_img_path.c_str(), "rb");
            if (!f) return -EIO;

            // LBA konumunu hesapla (Her LBA 512 byte) + offset
            uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512 + offset;
            fseek(f, file_pos, SEEK_SET);
            size_t bytes_read = fread(buf, 1, size, f);
            fclose(f);

            return bytes_read;
        }
    }
    return -ENOENT;
}

static int kms_fuse_write(const char* path, const char* buf, size_t size, off_t offset,
                          struct fuse_file_info* fi) {
    (void) fi;

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            FILE* f = fopen(g_img_path.c_str(), "r+b");
            if (!f) return -EIO;

            // LBA konumu + yazma offseti
            uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512 + offset;
            fseek(f, file_pos, SEEK_SET);
            size_t bytes_written = fwrite(buf, 1, size, f);
            fclose(f);

            // Eğer dosya boyutu büyüdüyse meta veriyi güncelle
            if (offset + bytes_written > g_meta.ent[i].size) {
                g_meta.ent[i].size = offset + bytes_written;
                
                // Basit bir sonraki boş LBA yönetimi (Fragmentasyonu şimdilik boş veriyoruz)
                uint32_t sectors_used = (g_meta.ent[i].size + 511) / 512;
                if (g_meta.ent[i].start_lba + sectors_used > g_meta.next_free_lba) {
                    g_meta.next_free_lba = g_meta.ent[i].start_lba + sectors_used;
                }
                save_meta();
            }

            return bytes_written;
        }
    }
    return -ENOENT;
}

static const struct fuse_operations kms_oper = {
    .getattr = kms_fuse_getattr,
    .unlink  = kms_fuse_unlink,
    .read    = kms_fuse_read,
    .write   = kms_fuse_write,
    .readdir = kms_fuse_readdir,
    .create  = kms_fuse_create,
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Kullanım:\n"
                  << "  Mount etmek için:   kms -m /path/to/file.img /path/to/directory\n"
                  << "  Unmount etmek için: kms -u /path/to/directory\n";
        return 1;
    }

    std::string mode = argv[1];

    // 🔹 UNMOUNT MODU (-u)
    if (mode == "-u") {
        std::string target_dir = argv[2];
        std::string cmd = "fusermount3 -u " + target_dir;
        std::cout << "[*] " << target_dir << " bağlantısı kesiliyor..." << std::endl;
        int res = std::system(cmd.c_str());
        if (res == 0) {
            std::cout << "[+] Başarıyla unmount edildi." << std::endl;
        } else {
            std::cerr << "[-] Hata: Unmount edilemedi!" << std::endl;
        }
        return res;
    }

    // 🔹 MOUNT MODU (-m)
    if (mode == "-m" && argc >= 4) {
        g_img_path = argv[2];
        char* mount_dir = argv[3];

        if (!load_meta()) {
            std::cerr << "[-] HATA: Geçerli bir imaj bulunamadı veya formatı hatalı!" << std::endl;
            return 1;
        }

        kms.Mount("/", 0, FileSystemType::KuvixFS);
        std::cout << "[+] " << g_img_path << " imajı " << mount_dir << " konumuna bağlanıyor..." << std::endl;

        // FUSE için argümanları elle simüle ediyoruz (ProgramAdı ve MountKlasörü şeklinde)
        char* fuse_argv[] = { argv[0], mount_dir, (char*)"-d" }; 
        // Not: "-d" parametresini arka planda debug/log görmek istersen bırakabilirsin, 
        // tamamen arka planda sessiz çalışsın istersen int fuse_argc = 2 yapıp "-d"'yi silebilirsin.
        
        return fuse_main(2, fuse_argv, &kms_oper, NULL);
    }

    return 1;
}