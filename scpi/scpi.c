/*-
 * Copyright (c) 2012-2013 Jan Breuer,
 *
 * All Rights Reserved
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   scpi.c
 * @date   Thu Nov 15 10:58:45 UTC 2012
 * 
 * @brief  SCPI parser implementation
 * 
 * 
 */

#include "scpi.h"
#include "scpi_utils.h"
#include "scpi_error.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

static size_t patternSeparatorPos(const char * pattern, size_t len);
static size_t patternSeparatorShortPos(const char * pattern, size_t len);
static size_t cmdSeparatorPos(const char * cmd, size_t len);
static size_t cmdTerminatorPos(const char * cmd, size_t len);
static size_t cmdlineSeparatorPos(const char * cmd, size_t len);
static char * cmdlineSeparator(const char * cmd, size_t len);
static char * cmdlineTerminator(const char * cmd, size_t len);
static const char * cmdlineNext(const char * cmd, size_t len);
static bool_t cmdMatch(const char * pattern, const char * cmd, size_t len);
static size_t skipWhitespace(const char * cmd, size_t len);

static void paramSkipBytes(scpi_context_t * context, size_t num);
static void paramSkipWhitespace(scpi_context_t * context);
static bool_t paramNext(scpi_context_t * context, bool_t mandatory);

#define max(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

/*
int _strnicmp(const char* s1, const char* s2, size_t len) {
    int result = 0;
    int i;

    for (i = 0; i < len && s1[i] && s2[i]; i++) {
        char c1 = tolower(s1[i]);
        char c2 = tolower(s2[i]);
        if (c1 != c2) {
            result = (int) c1 - (int) c2;
            break;
        }
    }

    return result;
}
 */

/**
 * Find pattern separator position
 * @param pattern
 * @param len - max search length
 * @return position of separator or len
 */
size_t patternSeparatorPos(const char * pattern, size_t len) {

    char * separator = strnpbrk(pattern, len, "?:[]");
    if (separator == NULL) {
        return len;
    } else {
        return separator - pattern;
    }
}

/**
 * Pattern is composed from upper case an lower case letters. This function
 * search the first lowercase letter
 * @param pattern
 * @param len - max search length
 * @return position of separator or len
 */
size_t patternSeparatorShortPos(const char * pattern, size_t len) {
    size_t i;
    for (i = 0; (i < len) && pattern[i]; i++) {
        if (islower(pattern[i])) {
            return i;
        }
    }
    return i;
}

/**
 * Find command separator position
 * @param cmd - input command
 * @param len - max search length
 * @return position of separator or len
 */
size_t cmdSeparatorPos(const char * cmd, size_t len) {
    char * separator = strnpbrk(cmd, len, ":?");
    size_t result;
    if (separator == NULL) {
        result = len;
    } else {
        result = separator - cmd;
    }

    return result;
}

/**
 * Find command termination character
 * @param cmd - input command
 * @param len - max search length
 * @return position of terminator or len
 */
size_t cmdTerminatorPos(const char * cmd, size_t len) {
    char * terminator = strnpbrk(cmd, len, "; \r\n\t");
    if (terminator == NULL) {
        return len;
    } else {
        return terminator - cmd;
    }
}

/**
 * Find command line separator
 * @param cmd - input command
 * @param len - max search length
 * @return pointer to line separator or NULL
 */
char * cmdlineSeparator(const char * cmd, size_t len) {
    return strnpbrk(cmd, len, ";\r\n");
}

/**
 * Find command line terminator
 * @param cmd - input command
 * @param len - max search length
 * @return pointer to command line terminator or NULL
 */
char * cmdlineTerminator(const char * cmd, size_t len) {
    return strnpbrk(cmd, len, "\r\n");
}

/**
 * Find command line separator position
 * @param cmd - input command
 * @param len - max search length
 * @return position of line separator or len
 */
size_t cmdlineSeparatorPos(const char * cmd, size_t len) {
    char * separator = cmdlineSeparator(cmd, len);
    if (separator == NULL) {
        return len;
    } else {
        return separator - cmd;
    }
}

/**
 * Find next part of command
 * @param cmd - input command
 * @param len - max search length
 * @return Pointer to next part of command
 */
const char * cmdlineNext(const char * cmd, size_t len) {
    const char * separator = cmdlineSeparator(cmd, len);
    if (separator == NULL) {
        return cmd + len;
    } else {
        return separator + 1;
    }
}

/**
 * Compare pattern and command
 * @param pattern
 * @param cmd - command
 * @param len - max search length
 * @return TRUE if pattern matches, FALSE otherwise
 */
bool_t cmdMatch(const char * pattern, const char * cmd, size_t len) {
    int result = FALSE;

    const char * pattern_ptr = pattern;
    int pattern_len = strlen(pattern);
    const char * pattern_end = pattern + pattern_len;

    const char * cmd_ptr = cmd;
    size_t cmd_len = strnlen(cmd, len);
    const char * cmd_end = cmd + cmd_len;

    while (1) {
        int pattern_sep_pos = patternSeparatorPos(pattern_ptr, pattern_end - pattern_ptr);
        int cmd_sep_pos = cmdSeparatorPos(cmd_ptr, cmd_end - cmd_ptr);
        int pattern_sep_pos_short = patternSeparatorShortPos(pattern_ptr, pattern_sep_pos);

        if (compareStr(pattern_ptr, pattern_sep_pos, cmd_ptr, cmd_sep_pos) ||
                compareStr(pattern_ptr, pattern_sep_pos_short, cmd_ptr, cmd_sep_pos)) {
            pattern_ptr = pattern_ptr + pattern_sep_pos;
            cmd_ptr = cmd_ptr + cmd_sep_pos;
            result = TRUE;

            // command is complete
            if ((pattern_ptr == pattern_end) && (cmd_ptr >= cmd_end)) {
                break;
            }

            // pattern complete, but command not
            if ((pattern_ptr == pattern_end) && (cmd_ptr < cmd_end)) {
                result = FALSE;
                break;
            }

            // command complete, but pattern not
            if (cmd_ptr >= cmd_end) {
                result = FALSE;
                break;
            }

            // both command and patter contains command separator at this position
            if ((pattern_ptr[0] == cmd_ptr[0]) && ((pattern_ptr[0] == ':') || (pattern_ptr[0] == '?'))) {
                pattern_ptr = pattern_ptr + 1;
                cmd_ptr = cmd_ptr + 1;
            } else {
                result = FALSE;
                break;
            }
        } else {
            result = FALSE;
            break;
        }
    }

    return result;
}

/**
 * Count white spaces from the beggining
 * @param cmd - command
 * @param len - max search length
 * @return number of white spaces
 */
size_t skipWhitespace(const char * cmd, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (!isspace(cmd[i])) {
            return i;
        }
    }
    return len;
}

/**
 * Write data to SCPI output
 * @param context
 * @param data
 * @param len - lenght of data to be written
 * @return number of bytes written
 */
static inline size_t writeData(scpi_context_t * context, const char * data, size_t len) {
    return context->interface->write(context, data, len);
}

/**
 * Write result delimiter to output
 * @param context
 * @return number of bytes written
 */
static inline size_t writeDelimiter(scpi_context_t * context) {
    if (context->output_count > 0) {
        return writeData(context, ", ", 2);
    } else {
        return 0;
    }
}

/**
 * Zapis nove radky na SCPI vystup
 * @param context
 * @return pocet zapsanych znaku
 */
static inline size_t writeNewLine(scpi_context_t * context) {
    if (context->output_count > 0) {
        return writeData(context, "\r\n", 2);
    } else {
        return 0;
    }
}

/**
 * Parse one command line
 * @param context
 * @param data - complete command line
 * @param len - command line length
 * @return 1 if the last evaluated command was found
 */
int SCPI_Parse(scpi_context_t * context, const char * data, size_t len) {
    int32_t i;
    int result = 0;
    const char * cmdline_end = data + len;
    const char * cmdline_ptr = data;
    size_t cmd_len;
    if (context == NULL) {
        return -1;
    }

    while (cmdline_ptr < cmdline_end) {

        result = 0;
        cmd_len = cmdTerminatorPos(cmdline_ptr, cmdline_end - cmdline_ptr);
        if (cmd_len > 0) {
            for (i = 0; context->cmdlist[i].pattern != NULL; i++) {
                if (cmdMatch(context->cmdlist[i].pattern, cmdline_ptr, cmd_len)) {
                    if (context->cmdlist[i].callback != NULL) {
                        context->paramlist.cmd = &context->cmdlist[i];
                        context->paramlist.parameters = cmdline_ptr + cmd_len;
                        context->paramlist.length = cmdlineSeparatorPos(context->paramlist.parameters, cmdline_end - context->paramlist.parameters);
                        context->output_count = 0;
                        context->input_count = 0;

                        SCPI_DEBUG_COMMAND(context);
                        context->cmdlist[i].callback(context);

                        writeNewLine(context); // conditionaly write new line

                        paramSkipWhitespace(context);
                        if (context->paramlist.length != 0) {
                            SCPI_ErrorPush(context, SCPI_ERROR_PARAMETER_NOT_ALLOWED);
                        }

                        result = 1;
                        break;
                    }
                }
            }
            if (result == 0) {
                SCPI_ErrorPush(context, SCPI_ERROR_UNDEFINED_HEADER);
            }
        }
        cmdline_ptr = cmdlineNext(cmdline_ptr, cmdline_end - cmdline_ptr);

    }
    return result;
}

/**
 * Initialize SCPI context structure
 * @param context
 * @param command_list
 * @param buffer
 * @param interface
 */
void SCPI_Init(scpi_context_t * context, scpi_command_t * command_list, scpi_buffer_t * buffer, scpi_interface_t * interface) {
    context->cmdlist = command_list;
    context->buffer.data = buffer->data;
    context->buffer.length = buffer->length;
    context->buffer.position = 0;
    context->interface = interface;
}

/**
 * Interface to the application. Adds data to system buffer and try to search
 * command line termination. If the termination is found or if len=0, command
 * parser is called.
 * 
 * @param context
 * @param data - data to process
 * @param len - length of data
 * @return 
 */
int SCPI_Input(scpi_context_t * context, const char * data, size_t len) {
    int result = 0;
    size_t buffer_free;
    char * cmd_term;
    int ws;
    if (len == 0) {
        context->buffer.data[context->buffer.position] = 0;
        result = SCPI_Parse(context, context->buffer.data, context->buffer.position);
        context->buffer.position = 0;
    } else {
        buffer_free = context->buffer.length - context->buffer.position;
        if (len > (buffer_free - 1)) {
            return -1;
        }
        memcpy(&context->buffer.data[context->buffer.position], data, len);
        context->buffer.position += len;
        context->buffer.data[context->buffer.position] = 0;

        ws = skipWhitespace(context->buffer.data, context->buffer.position);
        cmd_term = cmdlineTerminator(context->buffer.data + ws, context->buffer.position - ws);
        if (cmd_term != NULL) {
            int curr_len = cmd_term - context->buffer.data;
            result = SCPI_Parse(context, context->buffer.data + ws, curr_len - ws);
            memmove(context->buffer.data, cmd_term, context->buffer.position - curr_len);
            context->buffer.position -= curr_len;
        }
    }

    return result;
}

/**
 * Debug function: show current command and its parameters
 * @param context
 * @return 
 */
bool_t SCPI_DebugCommand(scpi_context_t * context) {
    (void) context;
    printf("**DEBUG: %s (\"", context->paramlist.cmd->pattern);
    fwrite(context->paramlist.parameters, 1, context->paramlist.length, stdout);
    printf("\" - %ld)\r\n", context->paramlist.length);

    return TRUE;
}


/* writing results */

/**
 * Write raw string result to the output
 * @param context
 * @param data
 * @return 
 */
size_t SCPI_ResultString(scpi_context_t * context, const char * data) {
    size_t len = strlen(data);
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, data, len);
    context->output_count++;
    return result;
}

/**
 * Write integer value to the result
 * @param context
 * @param val
 * @return 
 */
size_t SCPI_ResultInt(scpi_context_t * context, int32_t val) {
    char buffer[12];
    size_t result = 0;
    size_t len = longToStr(val, buffer, sizeof (buffer));
    result += writeDelimiter(context);
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;
}

/**
 * Write double walue to the result
 * @param context
 * @param val
 * @return 
 */
size_t SCPI_ResultDouble(scpi_context_t * context, double val) {
    char buffer[32];
    size_t result = 0;
    size_t len = doubleToStr(val, buffer, sizeof (buffer));
    result += writeDelimiter(context);
    result += writeData(context, buffer, len);
    context->output_count++;
    return result;

}

/**
 * Write string withn " to the result
 * @param context
 * @param data
 * @return 
 */
size_t SCPI_ResultText(scpi_context_t * context, const char * data) {
    size_t result = 0;
    result += writeDelimiter(context);
    result += writeData(context, "\"", 1);
    result += writeData(context, data, strlen(data));
    result += writeData(context, "\"", 1);
    context->output_count++;
    return result;
}

/* parsing parameters */

/**
 * Skip num bytes from the begginig of parameters
 * @param context
 * @param num
 */
void paramSkipBytes(scpi_context_t * context, size_t num) {
    if (context->paramlist.length < num) {
        num = context->paramlist.length;
    }
    context->paramlist.parameters += num;
    context->paramlist.length -= num;
}

/**
 * Skip white spaces from the beggining of parameters
 * @param context
 */
void paramSkipWhitespace(scpi_context_t * context) {
    size_t ws = skipWhitespace(context->paramlist.parameters, context->paramlist.length);
    paramSkipBytes(context, ws);
}

/**
 * Find next parameter
 * @param context
 * @param mandatory
 * @return 
 */
bool_t paramNext(scpi_context_t * context, bool_t mandatory) {
    paramSkipWhitespace(context);
    if (context->paramlist.length == 0) {
        if (mandatory) {
            SCPI_ErrorPush(context, SCPI_ERROR_MISSING_PARAMETER);
        }
        return FALSE;
    }
    if (context->input_count != 0) {
        if (context->paramlist.parameters[0] == ',') {
            paramSkipBytes(context, 1);
            paramSkipWhitespace(context);
        } else {
            SCPI_ErrorPush(context, SCPI_ERROR_INVALID_SEPARATOR);
            return FALSE;
        }
    }
    context->input_count++;
    return TRUE;
}

/**
 * Parse integer parameter
 * @param context
 * @param value
 * @param mandatory
 * @return 
 */
bool_t SCPI_ParamInt(scpi_context_t * context, int32_t * value, bool_t mandatory) {
    size_t len;

    if (!paramNext(context, mandatory)) {
        return FALSE;
    }
    len = strToLong(context->paramlist.parameters, value);

    if (len == 0) {
        if (mandatory) {
            SCPI_ErrorPush(context, SCPI_ERROR_SYNTAX);
        }
        return FALSE;
    } else {
        paramSkipBytes(context, len);
    }

    return TRUE;
}

/**
 * Parse double parameter
 * @param context
 * @param value
 * @param mandatory
 * @return 
 */
bool_t SCPI_ParamDouble(scpi_context_t * context, double * value, bool_t mandatory) {
    size_t len;

    if (!paramNext(context, mandatory)) {
        return FALSE;
    }
    len = strToDouble(context->paramlist.parameters, value);

    if (len == 0) {
        if (mandatory) {
            SCPI_ErrorPush(context, SCPI_ERROR_SYNTAX);
        }
        return FALSE;
    } else {
        paramSkipBytes(context, len);
    }

    return TRUE;
}

/**
 * Parse string parameter
 * @param context
 * @param value
 * @param len
 * @param mandatory
 * @return 
 */
bool_t SCPI_ParamString(scpi_context_t * context, char ** value, size_t * len, bool_t mandatory) {
    (void)context;
    (void)value;
    (void)len;
    (void)mandatory;
    
    return FALSE;
}