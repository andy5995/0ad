#!/bin/bash

sudo apt-get install -y software-properties-common
sudo bash -c "LC_ALL=C.UTF-8 add-apt-repository -y ppa:ondrej/php"
sudo apt-get update
sudo apt-get install -y libsodium-dev

# required to install libsdl2-dev
# https://github.com/travis-ci/travis-ci/issues/9065
sudo apt-get install libegl1-mesa-dev libgles2-mesa-dev

sudo apt-get install libboost-dev libboost-filesystem-dev   \
    libcurl4-gnutls-dev libenet-dev libicu-dev    \
    libminiupnpc-dev libnspr4-dev libnvtt-dev libogg-dev libopenal-dev   \
    libpng-dev libsdl2-dev libvorbis-dev libwxgtk3.0-dev libxcursor-dev  \
    libxml2-dev zlib1g-dev

# the gloox library on trusty is not compatible with the current build
# of 0ad
mkdir gloox
wget https://camaya.net/download/gloox-1.0.21.tar.bz2
tar xf gloox-1.0.21.tar.bz2
cd gloox-1.0.21

# reduce travis log by outputting to /dev/null
./configure > /dev/null
make > /dev/null && sudo make install
cd ../..
