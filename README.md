# README

曾千洋 2018011372



编译源程序：

```
gcc -Wall ufs.c `pkg-config fuse3 --cflags --libs` -o ufs
```

创建磁盘

```
rm /tmp/disk
touch /tmp/disk
fallocate --length=16g /tmp/disk
```

开启ufs、挂载在文件夹mount：

```
./ufs -d mount
```



```
bash script/test.sh
```





一些测试代码：

```
cd mount
ls
mkdir dir0
cd ../

ls
cd dir0
ls
cd ../
mkdir dir1
ls
echo Hello World >> file007
cat file007
```



