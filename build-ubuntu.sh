sudo apt-get update
sudo apt-get install -y build-essential autotools-dev pkgconf autoconf automake libtool m4 autoconf-archive \
    libevent-dev libzstd-dev
./autogen.sh
./configure --with-zstd --with-libevent
make
