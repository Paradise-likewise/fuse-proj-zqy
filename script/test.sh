set -e
set -o xtrace

gcc -Wall ufs.c `pkg-config fuse3 --cflags --libs` -o ufs

rm /tmp/disk # mark it if /tmp/disk not exist
touch /tmp/disk
fallocate --length=2g /tmp/disk

# fusermount -u mount

rm -rf mount # mark it if mount not exist
mkdir mount
./ufs -d mount


