#! /bin/sh
 
SOURCE_DIR=html-source/
DEST_DIR=html/

if test ! -e ${DEST_DIR}; then
    mkdir $DEST_DIR;
fi

FILES1="doc.html"
FILES2="install.html configure.html programs.html technical.html help.html"

echo -n "Pass1:"
for FILE in $FILES1; do
    {
    echo -n " $FILE"
    cat $SOURCE_DIR/head1.html > $FILE
    cat $SOURCE_DIR/$FILE     >> $FILE
    cat $SOURCE_DIR/foot.html >> $FILE
    }
done
echo

echo -n "Pass2:"
for FILE in $FILES2; do
    {
    echo -n " $FILE"
    cat $SOURCE_DIR/head2.html > $DEST_DIR/$FILE
    cat $SOURCE_DIR/$FILE     >> $DEST_DIR/$FILE
    cat $SOURCE_DIR/foot.html >> $DEST_DIR/$FILE
    }
done
echo

