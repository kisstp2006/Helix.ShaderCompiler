# Writes a binary file as a C++ byte array: cmake -DINPUT=file -DOUTPUT=header -DNAME=identifier -P EmbedFile.cmake
cmake_minimum_required(VERSION 3.28)
file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "#pragma once\n\ninline constexpr unsigned char ${NAME}[] = {\n")
file(APPEND "${OUTPUT}" "${bytes}")
file(APPEND "${OUTPUT}" "\n};\n")
