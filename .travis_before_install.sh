#!/bin/bash

sudo apt-get install -y software-properties-common
sudo bash -c "LC_ALL=C.UTF-8 add-apt-repository -y ppa:ondrej/php"
sudo apt-get update
sudo apt-get install -y libsodium-dev

sudo apt-get install libboost-dev libboost-filesystem-dev   \
    libcurl4-gnutls-dev libenet-dev libgloox-dev libicu-dev    \
    libminiupnpc-dev libnspr4-dev libnvtt-dev libogg-dev libopenal-dev   \
    libpng-dev libsdl2-dev libvorbis-dev libwxgtk3.0-dev libxcursor-dev  \
    libxml2-dev zlib1g-dev
