#!/usr/bin/env bash

if [[ ${TRAVIS_OS_NAME} == "linux" ]]; then

	source setup_ubuntu1204.sh

elif [[ ${TRAVIS_OS_NAME} == "osx" ]]; then

	source setup_osx.sh

fi

git clone https://github.com/smtrat/carl.git
mkdir carl/build || return 1
cd carl/build/ || return 1
cmake -D DEVELOPER=ON ../ || return 1
make resources -j1 || return 1
make -j1 lib_carl || return 1

cd ../../
