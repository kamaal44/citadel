#!/bin/bash

echo Updating webcit.pot from strings in the source code ...
xgettext \
	--copyright-holder='The Citadel Project - http://www.citadel.org' \
        --from-code='utf-8' \
	-k_ \
	-o webcit.pot \
	--add-comments \
	../*.c ../static/t/*.html

for x in *.po
do
	echo Merging webcit.pot into $x ...
	msgmerge $x webcit.pot -o $x
done