const char *const FUZZ_SEEDS[] = {
    "{\"a\":1,\"b\":[true,false,null],\"c\":\"str\\u00e9\\n\",\"d\":{\"e\":-1.5e10}}",
    "[1,2,3,[[[]]],{}]",
    "\"\\ud83d\\ude00 emoji\"",
    "{\"id\":\"td_1\",\"title\":\"Привет\",\"done\":false,\"updated_at\":1730000000000}",
    "-0.0e-7",
    0,
};
const char *const FUZZ_DICT[] = { "\\u", "\\ud83d", "\\\"", "1e999", "-", "[", "]", "{", "}", ":", ",", "null", "true", "\"", 0 };
