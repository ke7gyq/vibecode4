#ifndef PARSER_H
#define PARSER_H

#include <stdint.h>

/**
 * Token structure for command parsing
 * Defines a command token with associated metadata and handler function
 */
struct s_tokens {
    const char *tokenText;                          /* Command token text */
    const char *helpText;                           /* Help description for this command */
    uint8_t (*fn)(char *rest, void *v);            /* Function pointer to token handler */
};

/**
 * Parse a command string and execute corresponding token function
 * Extracts the first non-whitespace token from the input string and
 * searches for a matching token in the tokens array. If found, calls
 * the associated function with the remaining string.
 * 
 * @param buffer The input command string to parse
 * @param v Void pointer to pass context to token functions
 * @return Result code from the token function, or non-zero if token not found
 */
uint8_t parse(char *buffer, void *v);

#endif // PARSER_H
