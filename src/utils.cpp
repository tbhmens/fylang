#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#pragma once
static bool streq(const char *a, const unsigned int alen, const char *b, const unsigned int blen)
{
    if (alen != blen)
        return false;
    for (unsigned int i = 0; i < alen; i++)
        if (a[i] != b[i])
            return false;
    return true;
}
// assumes that num_str actually has that base
static unsigned int parse_pos_int(char *num_str, unsigned int num_str_len, unsigned int base = 10)
{
    unsigned int result = 0;
    for (unsigned int i = 0; i < num_str_len; i++)
        result = result * base + num_str[i] - '0';
    return result;
}
void error(const char *str)
{
    fprintf(stderr, "Error: %s\n", str);
    exit(1);
}