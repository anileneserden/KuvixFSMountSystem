#!/bin/bash

MNT_DIR="mnt"
IMG_FILE="disk.img"

# Klasör yoksa oluştur, varsa bağlantısını kopar (temizlik)
mkdir -p "$MNT_DIR"
fuse_unmount() {
    echo "[*] Bağlantı kesiliyor (unmount)..."
    fusermount3 -u "$MNT_DIR" 2>/dev/null
}
trap fuse_unmount EXIT

fuse_unmount
make clean && make

echo "[*] FUSE ile imaj mnt/ klasörüne bağlanıyor..."
# Arka planda (foreground modu iptal ederek) mnt klasörüne mount et
./kms_test "$MNT_DIR"

echo "[+] BAŞARILI! Şimdi başka bir terminal açıp mnt/ klasöründe çalışabilirsin."
echo "[*] Çıkmak ve bağlantıyı güvenli kapatmak için bu terminalde CTRL+C yap."

# Terminali açık tut ki logları görebilelim
while true; do sleep 1; done