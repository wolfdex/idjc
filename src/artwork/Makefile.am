SUBDIRS = lighttheme darktheme

dist_pkgdata_DATA = greenphone.png jack2.png \
led_lit_amber_black_border_64x64.png \
led_lit_green_black_border_64x64.png led_lit_red_black_border_64x64.png \
led_unlit_clear_border_64x64.png play3.png rec.png redphone.png \
logo.png icon.svg \
mic_on.png \
mic_off.png mic_unshown.png mic4.png

nodist_appicon_DATA = ${PACKAGE_NAME}.svg
appicondir = ${prefix}/share/icons/hicolor/scalable/apps

${PACKAGE_NAME}.svg: icon.svg
	@cp $? $@

clean-local:
	rm -f ${PACKAGE_NAME}.svg
