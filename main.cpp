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

// 🔹 FUSE Çağrıları
static int kms_fuse_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void) fi;
    std::memset(stbuf, 0, sizeof(struct stat));
    
    // Test aşamasında Dolphin kilidini açmak için izinleri 0777 yapıyoruz
    if (std::strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0777; // 0755 -> 0777 yapıldı
        stbuf->st_nlink = 2;
        return 0;
    }
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            if (g_meta.ent[i].size == KVX_DIR_SIZE) {
                stbuf->st_mode = S_IFDIR | 0777; // 0755 -> 0777 yapıldı
                stbuf->st_nlink = 2;
            } else {
                stbuf->st_mode = S_IFREG | 0666; // 0644 -> 0666 yapıldı
                stbuf->st_nlink = 1;
                stbuf->st_size = g_meta.ent[i].size;
            }
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_mkdir(const char* path, mode_t mode) {
    (void) mode;
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (!g_meta.ent[i].used) {
            std::strncpy(g_meta.ent[i].path, path, 63);
            g_meta.ent[i].size = KVX_DIR_SIZE; // KuvixFS mantığında bu bir klasördür
            g_meta.ent[i].start_lba = 0;       // Klasörlerin LBA'sı olmaz
            g_meta.ent[i].used = 1;
            g_meta.file_count++;
            save_meta();
            return 0;
        }
    }
    return -ENOSPC;
}

static int kms_fuse_rename(const char* from, const char* to, unsigned int flags) {
    // FUSE 3 standartlarında flags gelebilir, RENAME_NOREPLACE gibi durumlar için.
    // Şimdilik basitçe ezici adlandırma yapıyoruz.
    (void) flags;

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, from) == 0) {
            // Sadece meta tablodaki dosya yolunu (path) yenisiyle güncelliyoruz
            std::strncpy(g_meta.ent[i].path, to, 63);
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_truncate(const char* path, off_t size, struct fuse_file_info* fi) {
    (void) fi;

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            g_meta.ent[i].size = (uint32_t)size; // Dosya boyutunu Linux'un istediği boyuta (örn: 0) çekiyoruz
            
            // Eğer dosya tamamen sıfırlandıysa, imaj dosyasındaki ilgili alanı da temizleyebiliriz
            if (size == 0) {
                FILE* f = fopen(g_img_path.c_str(), "r+b");
                if (f) {
                    uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512;
                    fseek(f, file_pos, SEEK_SET);
                    // İlk sektörü sıfırla
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
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        // Dosya bulunduysa ve kullanımdaysa
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            
            // 1. İmaj dosyasındaki kapladığı alanı (sektörleri) temizleyelim (sıfırlayalım)
            FILE* f = fopen(g_img_path.c_str(), "r+b");
            if (f) {
                uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512;
                uint32_t sectors_used = (g_meta.ent[i].size + 511) / 512;
                
                fseek(f, file_pos, SEEK_SET);
                char zero_buf[512] = {0};
                for (uint32_t s = 0; s < sectors_used; s++) {
                    fwrite(zero_buf, 1, 512, f);
                }
                fclose(f);
            }

            // 2. Meta tablodaki girdiyi ve genel dosya sayacını güncelle
            g_meta.ent[i].used = 0;
            g_meta.ent[i].size = 0;
            g_meta.ent[i].start_lba = 0;
            std::memset(g_meta.ent[i].path, 0, sizeof(g_meta.ent[i].path));
            
            // 🔹 Kritik Düzeltme: Toplam dosya sayacını azaltıyoruz
            if (g_meta.file_count > 0) {
                g_meta.file_count--;
            }

            // 3. Değişiklikleri diske (meta alana) kaydet
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_rmdir(const char* path) {
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        // Klasör bulunduysa ve kullanımdaysa
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            
            // Güvenlik Kontrolü: Eğer bu bir dosya ise rmdir ile silinmesin
            if (g_meta.ent[i].size != KVX_DIR_SIZE) {
                return -ENOTDIR;
            }

            // Meta tablodaki girdiyi ve genel dosya sayacını güncelle
            g_meta.ent[i].used = 0;
            g_meta.ent[i].size = 0;
            g_meta.ent[i].start_lba = 0;
            std::memset(g_meta.ent[i].path, 0, sizeof(g_meta.ent[i].path));
            
            if (g_meta.file_count > 0) {
                g_meta.file_count--;
            }

            // Değişiklikleri diske kaydet
            save_meta();
            return 0;
        }
    }
    return -ENOENT;
}

static int kms_fuse_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void) offset; (void) fi; (void) flags;
    
    // Güvenlik Kontrolü
    MountPoint* mp = kms.ResolvePath(path);
    if (!mp) return -ENOENT;

    // Her dizinde bulunması gereken standart noktalar
    filler(buf, ".", NULL, 0, (fuse_fill_dir_flags)0);
    filler(buf, "..", NULL, 0, (fuse_fill_dir_flags)0);

    std::string current_dir(path);
    if (current_dir.back() != '/') {
        current_dir += "/";
    }

    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used) {
            std::string file_path(g_meta.ent[i].path);
            
            // Eğer aranan şey kök dizin ("/") ise ve dosya kök dizindeyse doğrudan ismi al
            if (current_dir == "/" && file_path != "/" && file_path.find('/', 1) == std::string::npos) {
                // Başındaki '/' işaretini atlayarak ekle
                filler(buf, file_path.c_str() + 1, NULL, 0, (fuse_fill_dir_flags)0);
                continue;
            }

            // Alt dizinler için kontrol: Dosya yolu, aradığımız dizinle mi başlıyor?
            if (file_path.rfind(current_dir, 0) == 0 && file_path != current_dir) {
                // Üst dizin kısmını kırpıp sadece dosya/klasör adını alıyoruz
                std::string sub_name = file_path.substr(current_dir.length());
                
                // Eğer içinde başka '/' yoksa, bu tam olarak bu dizinin altındaki elemandır
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

static int kms_fuse_read(const char* path, char* buf, size_t size, off_t offset,
                         struct fuse_file_info* fi) {
    (void) fi;
    
    for (int i = 0; i < KVX_MAX_FILES; i++) {
        if (g_meta.ent[i].used && std::strcmp(g_meta.ent[i].path, path) == 0) {
            // off_t türünü güvenle karşılaştırmak için uint64_t veya dosya boyutu tipine döküyoruz
            if ((uint32_t)offset >= g_meta.ent[i].size) return 0;
            if ((uint32_t)offset + size > g_meta.ent[i].size) {
                size = g_meta.ent[i].size - (uint32_t)offset;
            }

            FILE* f = fopen(g_img_path.c_str(), "rb");
            if (!f) return -EIO;

            uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512 + (uint64_t)offset;
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

            uint64_t file_pos = (uint64_t)g_meta.ent[i].start_lba * 512 + (uint64_t)offset;
            fseek(f, file_pos, SEEK_SET);
            size_t bytes_written = fwrite(buf, 1, size, f);
            fclose(f);

            if ((uint32_t)offset + bytes_written > g_meta.ent[i].size) {
                g_meta.ent[i].size = (uint32_t)offset + bytes_written;
                
                uint32_t sectors_used = (g_meta.ent[i].size + 511) / 512;
                if (g_meta.ent[i].start_lba + sectors_used > g_meta.next_free_lba) {
                    g_meta.next_free_lba = g_meta.ent[i].start_lba + sectors_used;
                }
            }
            save_meta();

            return bytes_written;
        }
    }
    return -ENOENT;
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

        // FUSE için argüman dizisini genişletiyoruz (İzin çakışmalarını çözmek adına)
        // allow_other,default_permissions eklenerek Dolphin/Sudo kilitleri çözüldü.
        char* fuse_argv[] = { 
            argv[0], 
            mount_dir, 
            (char*)"-o", 
            (char*)"allow_other" // default_permissions kelimesini kaldırdık
        };
        
        return fuse_main(4, fuse_argv, &kms_oper, NULL);
    }

    return 1;
}