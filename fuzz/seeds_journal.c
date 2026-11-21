/* "[snapshot]\x01" then journal lines; a line starting with '#' is written without a CRC prefix */
const char *const FUZZ_SEEDS[] = {
    "{\"seq\":1,\"entity\":\"items\",\"op\":\"put\",\"id\":\"a\",\"data\":{\"id\":\"a\"},\"ts\":1}\n"
    "{\"seq\":2,\"entity\":\"users\",\"op\":\"put\",\"id\":\"u\",\"data\":{\"id\":\"u\",\"x\":[1,2]},\"ts\":1}\n"
    "{\"seq\":3,\"entity\":\"items\",\"op\":\"del\",\"id\":\"a\",\"data\":null,\"ts\":1}\n",
    "{\"seq\":3,\"collections\":{\"items\":[{\"id\":\"a\",\"n\":1}],\"users\":[{\"id\":\"u\"}]}}\x01"
    "{\"seq\":4,\"entity\":\"items\",\"op\":\"put\",\"id\":\"b\",\"data\":{\"id\":\"b\"},\"ts\":1}\n",
    "{\"seq\":2,\"collections\":{\"items\":[]}}\x01#00000000 {\"seq\":3}\n",
    "garbage\n\n#zz\n",
    0,
};
const char *const FUZZ_DICT[] = { "\n", "{\"seq\":", "\"op\":\"put\"", "\"op\":\"del\"", "\"entity\":\"items\"",
                                  "\"entity\":\"users\"", "\"id\":\"b\"", "\"data\":{\"id\":\"b\"}", "#", "\x01", 0 };
