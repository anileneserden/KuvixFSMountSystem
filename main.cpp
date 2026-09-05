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

// 🔹 Hangi dosya sisteminin aktif olduğunu anlamak için enum ekliyoruz
enum class ActiveFS {
    Unknown,
    KuvixFS,
    KryonFS
};

ActiveFS g_active_fs = ActiveFS::Unknown;

// --- KUVIXFS YAPILARI ---
#define KVX_MAGIC     "KVXFS1"
#define KVX_MAX_FILES 256
#define KVX_DIR_SIZE  0xFFFFFFFFu

typedef struct {
    char     path[64];
    uint32_t start_lba;
    uint32_t size;      
    uint8_t  used;
    uint16_t permissions;
    uint8_t  owner_uid;
} __attribute__((packed)) kvx_ent_t;

typedef struct {
    char     magic[8];
    uint32_t file_count;
    uint32_t next_free_lba;
    kvx_ent_t ent[KVX_MAX_FILES];
} __attribute__((packed)) kvx_meta_t;

kvx_meta_t g_kvx_meta;

// --- KRYFS YAPILARI ---
#define KRYFS_MAGIC       0x4B525953 // "KRYS"
#define KRYFS_BLOCK_SIZE  512
#define KRYFS_MAX_INODES  16

typedef struct {
    uint32_t magic;
    uint32_t total_sectors;
    uint32_t inode_count;
    uint32_t block_size;
    char     volume_name[32];
} __attribute__((packed)) kryfs_superblock_t;

typedef struct {
    uint32_t inode_id;
    char     filename[32];
    uint32_t size;
    uint32_t first_block;
    uint8_t  is_used;
} __attribute__((packed)) kryfs_inode_t;

kryfs_superblock_t g_kry_sb;
kryfs_inode_t      g_kry_inodes[KRYFS_MAX_INODES];

KuvixFSMountSystem kms;
std::string g_img_path = "";

// 🔹 load_meta() fonksiyonunu iki formatı da kontrol edecek şekilde güncelliyoruz
bool load_meta() {
    FILE* f = fopen(g_img_path.c_str(), "rb");
    if (!f) return false;

    // 1. Önce KRYFS kontrolü yapalım (Byte 0)
    kryfs_superblock_t temp_kry_sb;
    fseek(f, 0, SEEK_SET);
    if (fread(&temp_kry_sb, sizeof(kryfs_superblock_t), 1, f) == 1) {
        std::cout << "[DEBUG] Okunan KRYFS Magic (Hex): 0x" << std::hex << temp_kry_sb.magic << std::dec << " (Beklenen: 0x" << std::hex << KRYFS_MAGIC << std::dec << ")" << std::endl;
        if (temp_kry_sb.magic == KRYFS_MAGIC) {
            g_kry_sb = temp_kry_sb;
            fseek(f, KRYFS_BLOCK_SIZE, SEEK_SET);
            fread(g_kry_inodes, sizeof(kryfs_inode_t), KRYFS_MAX_INODES, f);
            fclose(f);
            g_active_fs = ActiveFS::KryonFS;
            return true;
        }
    }

    // 2. KRYFS değilse KuvixFS kontrolü yapalım
    fseek(f, 2048 * 512, SEEK_SET);
    if (fread(&g_kvx_meta, sizeof(kvx_meta_t), 1, f) == 1) {
        std::cout << "[DEBUG] Okunan KuvixFS Magic: " << std::string(g_kvx_meta.magic, 6) << std::endl;
        if (std::strncmp(g_kvx_meta.magic, KVX_MAGIC, 6) == 0) {
            fclose(f);
            g_active_fs = ActiveFS::KuvixFS;
            return true;
        }
    }

    fclose(f);
    return false;
}

// 🔹 save_meta() fonksiyonunu aktif dosya sistemine göre yönlendiriyoruz
void save_meta() {
    FILE* f = fopen(g_img_path.c_str(), "r+b");
    if (!f) return;

    if (g_active_fs == ActiveFS::KryonFS) {
        fseek(f, 0, SEEK_SET);
        fwrite(&g_kry_sb, sizeof(kryfs_superblock_t), 1, f);
        fseek(f, KRYFS_BLOCK_SIZE, SEEK_SET);
        fwrite(g_kry_inodes, sizeof(kryfs_inode_t), KRYFS_MAX_INODES, f);
    } 
    else if (g_active_fs == ActiveFS::KuvixFS) {
        fseek(f, 2048 * 512, SEEK_SET);
        fwrite(&g_kvx_meta, sizeof(kvx_meta_t), 1, f);
    }

    fflush(f); 
    fsync(fileno(f)); 
    fclose(f);
}

static int kms_fuse_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void) fi;
    std::memset(stbuf, 0, sizeof(struct stat));
    
    if (std::strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0777;
        stbuf->st_uid = 0; 
        stbuf->st_gid = 0;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    // 🔹 KRYFS İÇİN GETATTR
    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used && std::strcmp(g_kry_inodes[i].filename, fname) == 0) {
                stbuf->st_uid = getuid();
                stbuf->st_gid = getgid();
                stbuf->st_mode = S_IFREG | 0644;
                stbuf->st_nlink = 1;
                stbuf->st_size = g_kry_inodes[i].size;
                return 0;
            }
        }
        return -ENOENT;
    }

    // 🔹 KUVIXFS İÇİN GETATTR (Mevcut kodun)
    if (g_active_fs == ActiveFS::KuvixFS) {
        for (int i = 0; i < KVX_MAX_FILES; i++) {
            if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
                stbuf->st_uid = g_kvx_meta.ent[i].owner_uid;
                stbuf->st_gid = 0;

                if (g_kvx_meta.ent[i].size == KVX_DIR_SIZE) {
                    stbuf->st_mode = S_IFDIR | 0777;
                    stbuf->st_nlink = 2;
                } else {
                    stbuf->st_mode = S_IFREG | g_kvx_meta.ent[i].permissions;
                    stbuf->st_nlink = 1;
                    stbuf->st_size = g_kvx_meta.ent[i].size;
                }
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int kms_fuse_mkdir(const char* path, mode_t mode) {
    if (g_active_fs == ActiveFS::KryonFS) return -ENOTSUP; // KRYFS düz yapıda olduğu için klasör desteklemiyor olabilir

    (void) mode;
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (!g_kvx_meta.ent[i].used) {
            std::strncpy(g_kvx_meta.ent[i].path, path, 63);
            g_kvx_meta.ent[i].size = KVX_DIR_SIZE;
            g_kvx_meta.ent[i].start_lba = 0;
            g_kvx_meta.ent[i].used = 1;
            g_kvx_meta.file_count++;
            save_meta();
            return 0;
        }
    }
    return -ENOSPC;
}

static int kms_fuse_rename(const char* from, const char* to, unsigned int flags) {
    if (g_active_fs == ActiveFS::KryonFS) return -ENOTSUP;
    (void) flags;

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, from) == 0) {
            std::strncpy(g_kvx_meta.ent[i].path, to, 63);
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_truncate(const char* path, off_t size, struct fuse_file_info* fi) {
    (void) fi;

    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used && std::strcmp(g_kry_inodes[i].filename, fname) == 0) {
                g_kry_inodes[i].size = (uint32_t)size;
                save_meta();
                return 0;
            }
        }
        return -ENOENT;
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
            g_kvx_meta.ent[i].size = (uint32_t)size;
            if (size == 0) {
                FILE* f = fopen(g_img_path.c_str(), "r+b");
                if (f) {
                    uint64_t file_pos = (uint64_t)g_kvx_meta.ent[i].start_lba * 512;
                    fseek(f, file_pos, SEEK_SET);
                    char zero_buf[512] = {0};
                    fwrite(zero_buf, 1, 512, f);
                    fclose(f);
                }
            }
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_unlink(const char* path) {
    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used && std::strcmp(g_kry_inodes[i].filename, fname) == 0) {
                g_kry_inodes[i].is_used = 0;
                g_kry_inodes[i].size = 0;
                std::memset(g_kry_inodes[i].filename, 0, sizeof(g_kry_inodes[i].filename));
                save_meta();
                return 0;
            }
        }
        return -ENOENT;
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
            FILE* f = fopen(g_img_path.c_str(), "r+b");
            if (f) {
                uint64_t file_pos = (uint64_t)g_kvx_meta.ent[i].start_lba * 512;
                uint32_t sectors_used = (g_kvx_meta.ent[i].size + 511) / 512;
                fseek(f, file_pos, SEEK_SET);
                char zero_buf[512] = {0};
                for (uint32_t s = 0; s < sectors_used; s++) {
                    fwrite(zero_buf, 1, 512, f);
                }
                fclose(f);
            }

            g_kvx_meta.ent[i].used = 0;
            g_kvx_meta.ent[i].size = 0;
            g_kvx_meta.ent[i].start_lba = 0;
            std::memset(g_kvx_meta.ent[i].path, 0, sizeof(g_kvx_meta.ent[i].path));
            
            if (g_kvx_meta.file_count > 0) {
                g_kvx_meta.file_count--;
            }

            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_rmdir(const char* path) {
    if (g_active_fs == ActiveFS::KryonFS) return -ENOTSUP;

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
            if (g_kvx_meta.ent[i].size != KVX_DIR_SIZE) {
                return -ENOTDIR;
            }

            g_kvx_meta.ent[i].used = 0;
            g_kvx_meta.ent[i].size = 0;
            g_kvx_meta.ent[i].start_lba = 0;
            std::memset(g_kvx_meta.ent[i].path, 0, sizeof(g_kvx_meta.ent[i].path));
            
            if (g_kvx_meta.file_count > 0) {
                g_kvx_meta.file_count--;
            }

            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;
    
    MountPoint* mp = kms.ResolvePath(path);
    if (!mp) return -ENOENT;

    filler(buf, ".", NULL, 0, (fuse_fill_dir_flags)0);
    filler(buf, "..", NULL, 0, (fuse_fill_dir_flags)0);

    // 🔹 KRYFS İÇİN READDDIR
    if (g_active_fs == ActiveFS::KryonFS) {
        if (std::strcmp(path, "/") != 0) return -ENOENT;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used) {
                filler(buf, g_kry_inodes[i].filename, NULL, 0, (fuse_fill_dir_flags)0);
            }
        }
        return 0;
    }

    // 🔹 KUVIXFS İÇİN READDDIR (Mevcut kodun)
    std::string current_dir(path);
    if (current_dir.back() != '/') {
        current_dir += "/";
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used) {
            std::string file_path(g_kvx_meta.ent[i].path);
            
            if (current_dir == "/" && file_path != "/" && file_path.find('/', 1) == std::string::npos) {
                filler(buf, file_path.c_str() + 1, NULL, 0, (fuse_fill_dir_flags)0);
                continue;
            }

            if (file_path.rfind(current_dir, 0) == 0 && file_path != current_dir) {
                std::string sub_name = file_path.substr(current_dir.length());
                if (sub_name.find('/') == std::string::npos && !sub_name.empty()) {
                    filler(buf, sub_name.c_str(), NULL, 0, (fuse_fill_dir_flags)0);
                }
            }
        }
    }
    return 0;
}

static int kms_fuse_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    (void) mode; (void) fi;

    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        // Dosya adı çok uzunsa engelle
        if (std::strlen(fname) >= 32) return -ENAMETOOLONG;

        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (!g_kry_inodes[i].is_used) {
                g_kry_inodes[i].inode_id = i + 1;
                std::strncpy(g_kry_inodes[i].filename, fname, 31);
                g_kry_inodes[i].filename[31] = '\0';
                g_kry_inodes[i].size = 0;
                // Her inode için 512 baytlık blok ayıralım (Superblock = Blok 0, Inode tablosu = Blok 1 vb.)
                // İtibar güvenliği için her dosyaya distinct bir başlangıç bloğu verelim:
                g_kry_inodes[i].first_block = 10 + (i * 4); 
                g_kry_inodes[i].is_used = 1;
                
                save_meta();
                std::cout << "[+] KRYFS Dosya Oluşturuldu: " << fname << " (Blok: " << g_kry_inodes[i].first_block << ")" << std::endl;
                return 0;
            }
        }
        return -ENOSPC;
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (!g_kvx_meta.ent[i].used) {
            std::strncpy(g_kvx_meta.ent[i].path, path, 63);
            g_kvx_meta.ent[i].size = 0;
            g_kvx_meta.ent[i].start_lba = g_kvx_meta.next_free_lba;
            g_kvx_meta.ent[i].used = 1;
            
            if (std::strcmp(path, "/etc/passwd") == 0) {
                g_kvx_meta.ent[i].owner_uid = 0;
                g_kvx_meta.ent[i].permissions = 0644;
            } else {
                g_kvx_meta.ent[i].owner_uid = 1000;
                g_kvx_meta.ent[i].permissions = 0666;
            }

            g_kvx_meta.file_count++;
            save_meta();
            return 0;
        }
    }
    return -ENOSPC;
}

static int kms_fuse_read(const char* path, char* buf, size_t size, off_t offset,
                         struct fuse_file_info* fi) {
    (void) fi;
    
    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used && std::strcmp(g_kry_inodes[i].filename, fname) == 0) {
                if ((uint32_t)offset >= g_kry_inodes[i].size) return 0;
                if ((uint32_t)offset + size > g_kry_inodes[i].size) {
                    size = g_kry_inodes[i].size - (uint32_t)offset;
                }

                FILE* f = fopen(g_img_path.c_str(), "rb");
                if (!f) return -EIO;

                uint64_t file_pos = (uint64_t)g_kry_inodes[i].first_block * KRYFS_BLOCK_SIZE + (uint64_t)offset;
                fseek(f, file_pos, SEEK_SET);
                size_t bytes_read = fread(buf, 1, size, f);
                fclose(f);
                return bytes_read;
            }
        }
        return -ENOENT;
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
            if ((uint32_t)offset >= g_kvx_meta.ent[i].size) return 0;
            if ((uint32_t)offset + size > g_kvx_meta.ent[i].size) {
                size = g_kvx_meta.ent[i].size - (uint32_t)offset;
            }

            FILE* f = fopen(g_img_path.c_str(), "rb");
            if (!f) return -EIO;

            uint64_t file_pos = (uint64_t)g_kvx_meta.ent[i].start_lba * 512 + (uint64_t)offset;
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

    if (g_active_fs == ActiveFS::KryonFS) {
        const char* fname = path + 1;
        for (int i = 0; i < KRYFS_MAX_INODES; i++) {
            if (g_kry_inodes[i].is_used && std::strcmp(g_kry_inodes[i].filename, fname) == 0) {
                FILE* f = fopen(g_img_path.c_str(), "r+b");
                if (!f) return -EIO;

                uint64_t file_pos = (uint64_t)g_kry_inodes[i].first_block * KRYFS_BLOCK_SIZE + (uint64_t)offset;
                fseek(f, file_pos, SEEK_SET);
                size_t bytes_written = fwrite(buf, 1, size, f);
                
                uint32_t new_end_offset = (uint32_t)offset + bytes_written;
                if (new_end_offset > g_kry_inodes[i].size) {
                    g_kry_inodes[i].size = new_end_offset;
                }

                fflush(f);
                fsync(fileno(f));
                fclose(f);

                // 🔹 KRİTİK: Inode boyutundaki değişimi diske kalıcı olarak kaydet!
                save_meta();
                
                std::cout << "[+] KRYFS Yazıldı: " << bytes_written << " bayt (" << path << ")" << std::endl;
                return bytes_written;
            }
        }
        return -ENOENT;
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_kvx_meta.ent[i].used && std::strcmp(g_kvx_meta.ent[i].path, path) == 0) {
            FILE* f = fopen(g_img_path.c_str(), "r+b");
            if (!f) return -EIO;

            uint64_t file_pos = (uint64_t)g_kvx_meta.ent[i].start_lba * 512 + (uint64_t)offset;
            fseek(f, file_pos, SEEK_SET);
            size_t bytes_written = fwrite(buf, 1, size, f);
            
            uint32_t old_size = g_kvx_meta.ent[i].size;
            uint32_t new_end_offset = (uint32_t)offset + bytes_written;

            if (new_end_offset > old_size) {
                g_kvx_meta.ent[i].size = new_end_offset;
                uint32_t sectors_used = (g_kvx_meta.ent[i].size + 511) / 512;
                if (g_kvx_meta.ent[i].start_lba + sectors_used > g_kvx_meta.next_free_lba) {
                    g_kvx_meta.next_free_lba = g_kvx_meta.ent[i].start_lba + sectors_used;
                }
            } else {
                uint64_t truncate_pos = (uint64_t)g_kvx_meta.ent[i].start_lba * 512 + new_end_offset;
                fseek(f, truncate_pos, SEEK_SET);
                uint32_t bytes_to_zero = old_size - new_end_offset;
                char zero_chunk[512] = {0};
                while (bytes_to_zero > 0) {
                    size_t write_now = (bytes_to_zero > 512) ? 512 : bytes_to_zero;
                    fwrite(zero_chunk, 1, write_now, f);
                    bytes_to_zero -= write_now;
                }
                g_kvx_meta.ent[i].size = new_end_offset;
            }

            fflush(f);
            fsync(fileno(f));
            fclose(f);
            save_meta();
            return bytes_written;
        }
    }
    return -ENOENT;
}

static int kms_fuse_utimens(const char* path, const struct timespec tv[2], struct fuse_file_info* fi) {
    (void) path; (void) tv; (void) fi;
    return 0;
}

static const struct fuse_operations kms_oper = []{
    struct fuse_operations op;
    std::memset(&op, 0, sizeof(op));
    op.getattr  = kms_fuse_getattr;
    op.mkdir    = kms_fuse_mkdir;
    op.rmdir    = kms_fuse_rmdir;
    op.unlink   = kms_fuse_unlink;
    op.rename   = kms_fuse_rename;
    op.truncate = kms_fuse_truncate; 
    op.readdir  = kms_fuse_readdir;
    op.create   = kms_fuse_create;
    op.read     = kms_fuse_read;
    op.write    = kms_fuse_write;
    op.utimens  = kms_fuse_utimens;
    return op;
}();

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Kullanım:\n"
                  << "  Mount etmek için:   kms -m /path/to/file.img /path/to/directory\n"
                  << "  Unmount etmek için: kms -u /path/to/directory\n";
        return 1;
    }

    std::string mode = argv[1];

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

    if (mode == "-m" && argc >= 4) {
        g_img_path = argv[2];
        char* mount_dir = argv[3];

        if (!load_meta()) {
            std::cerr << "[-] HATA: Geçerli bir imaj bulunamadı veya formatı hatalı!" << std::endl;
            return 1;
        }

        // Hangi dosya sistemi aktifse KMS'e ona göre bildiriyoruz
        if (g_active_fs == ActiveFS::KryonFS) {
            kms.Mount("/", 0, FileSystemType::KuvixFS); // Varsa KRYFS türünü ekleyebilirsin, yoksa şimdilik idare eder
            std::cout << "[*] Aktif Dosya Sistemi: KRYFS" << std::endl;
        } else {
            kms.Mount("/", 0, FileSystemType::KuvixFS);
            std::cout << "[*] Aktif Dosya Sistemi: KuvixFS" << std::endl;
        }

        std::cout << "[+] " << g_img_path << " imajı " << mount_dir << " konumuna bağlanıyor..." << std::endl;

        char* fuse_argv[] = { 
            argv[0], 
            mount_dir, 
            (char*)"-o", 
            (char*)"allow_other"
        };
        
        int fuse_res = fuse_main(4, fuse_argv, &kms_oper, NULL);
        if (fuse_res != 0) {
            std::cerr << "[-] FUSE Hata Kodu: " << fuse_res << std::endl;
        }
        return fuse_res;
    }

    return 1;
}