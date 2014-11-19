#!/bin/sh

# Dump an imagepile index and process the dump to display the contents

test ! -e "$1" && echo "Specify an imagepile hash index file." && exit 1

hexdump "$1" -xv | \
	cut -d\  -f2- | tr -d ' ' | \
	sed 's/\(....\)\(....\)\(....\)\(....\)\(....\)\(....\)\(....\)\(....\)/\4\3\2\1\n\8\7\6\5/g'
