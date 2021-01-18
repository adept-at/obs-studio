mkdir -p package
mkdir -p package/bin
mkdir -p package/plugins
mkdir -p package/libs
rm -rf package/libs/*

cp cmake_output/rundir/RelWithDebInfo/bin/* package/bin
cp cmake_output/rundir/RelWithDebInfo/obs-plugins/* package/plugins

#Remove things we don't need
rm package/bin/obs
rm package/plugins/frontend-tools.so
rm package/plugins/decklink-ouput-ui.so
rm package/bin/libobs-scripting.dylib
rm package/bin/libobs-frontend-api.dylib

../CI/install/osx/dylibBundler -b -cd -d package/libs \
-s ./package/bin \
-s /tmp/obsdeps/lib \
-x ./package/bin/obs-cli \
-x ./package/bin/obs-ffmpeg-mux \
-x ./package/plugins/coreaudio-encoder.so \
-x ./package/plugins/image-source.so \
-x ./package/plugins/mac-decklink.so \
-x ./package/plugins/obs-ffmpeg.so \
-x ./package/plugins/obs-transitions.so \
-x ./package/plugins/rtmp-services.so \
-x ./package/plugins/mac-avcapture.so \
-x ./package/plugins/mac-syphon.so \
-x ./package/plugins/obs-filters.so \
-x ./package/plugins/obs-vst.so \
-x ./package/plugins/text-freetype2.so \
-x ./package/plugins/mac-capture.so \
-x ./package/plugins/mac-vth264.so \
-x ./package/plugins/obs-outputs.so \
-x ./package/plugins/obs-x264.so

# Remove duplicate libobs. Everything now uses the one in package/libs
rm package/bin/libobs.0.dylib
