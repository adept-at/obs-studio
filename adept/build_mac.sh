tar -zxvf prebuilt/osx/macos-deps-2020-12-22.tar.gz -C /tmp
tar -zxvf prebuilt/osx/macos-qt-5.15.2-2020-12-22.tar.gz -C /tmp

mkdir cmake_output
cd cmake_output
cmake -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 -DQTDIR="/tmp/obsdeps" -DSWIGDIR="/tmp/obsdeps" -DDepsPath="/tmp/obsdeps" -DDISABLE_PYTHON=ON ../..
make

# Fix up rpaths for libraries
./fix-libs.sh

# Create zip file
version=`cat version.txt`
zip obs-libs-$version.zip -r package
