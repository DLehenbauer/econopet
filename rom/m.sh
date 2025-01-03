DIST="/Users/danlehen/.vscode/extensions/entan-gl.cc65-vice-8.2.0/dist"
export CC65_HOME="$DIST/cc65"
export PATH="$CC65_HOME/bin_darwin_x64:$PATH"
make
ls -l program.pet
ls -l rom.bin