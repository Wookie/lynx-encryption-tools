/* Atari Lynx Encryption Tool
 * Copyright (C) 2009 David Huseby <dave@linuxprogrammer.org>
 *
 * NOTES:
 *
 * This software is original software written completely by me, but there are
 * pieces of data (e.g. the keys.h and loaders.h files) that I got from the 
 * Atari Age Lynx Programming forum and from people in the Lynx community,
 * namely Karri Kaksonen.  Without their help, this would have never been
 * possible.  I was standing on the shoulders of giants.
 *
 * According to the documentation on RSA, the way the public/private 
 * exponents are related is that encryption works like so:
 * 
 * encrypted = (plaintext ^ private exponent) % public modulus
 *
 * decryption, which we already have working, works like this:
 *
 * plaintext = (encrypted ^ public exponent) % public modulus
 *
 * The keys.h file contains definitions for the Lynx public exponent, 
 * private exponent and the public modulus.
 * 
 * This app shows how to take a plaintext loader and pad it, encrypt
 * it, frame it and pack it into the encrypted version of his loader, all
 * using the C and the OpenSSL bignum library for the RSA step.
 * 
 * The trick is knowing how to properly frame the encrypted blocks.  
 * Harry's plaintext loader has two sections in it, one that starts at 
 * offset 0 and is 150 bytes long, and another that starts at offset 256 
 * and is 250 bytes long.  
 *
 * What I discovered is that the encrypted loader is broken up into frames.
 * Each frame starts with a single byte that specifies how many blocks are 
 * in the frame.  The frames are packed together without any padding between 
 * them.  The block count byte has the value 256 - block count.
 *
 * Another thing I discovered was that the unencrypted data is processed in
 * 50 byte chunks.  Each chunk is padded out to 51 bytes before being
 * encrypted using the private exponent and public modulus.
 *
 * LICENSE:
 *
 * This software is provided 'as-is', without any express or implied warranty. 
 * In no event will the authors be held liable for any damages arising from the 
 * use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose, 
 * including commercial applications, and to alter it and redistribute it 
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not 
 * claim that you wrote the original software. If you use this software in a 
 * product, an acknowledgment in the product documentation would be appreciated 
 * but is not required.
 * 
 * 2. Altered source versions must be plainly marked as such, and must not be 
 * misrepresented as being the original software.
 * 
 * 3. This notice may not be removed or altered from any source distribution. 
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <openssl/bn.h>
#include "sizes.h"
#include "keys.h"


typedef struct encrypted_frame_s
{
    int blocks;
    unsigned char data[MAX_ENCRYPTED_FRAME_SIZE];
} encrypted_frame_t;

typedef struct plaintext_frame_s
{
    int blocks;
    unsigned char data[MAX_PLAINTEXT_FRAME_SIZE];
} plaintext_frame_t;

typedef struct frame_def_s
{
    long offset;
    int blocks;
} frame_def_t;

#define min(x,y) ((x < y) ? x : y)
void print_data(const unsigned char * data, int size)
{
    int i = 0;
    int j, count;
    int left = size;

    while(i < size)
    {
        count = min(8, (size - i));

        printf("    ");
        for(j = 0; j < count; j++)
        {
            printf("0x%02x, ", data[i + j]);
        }
        printf("\n");
        i += count;
    }
}


/* This function pads and encrypts a single block of plaintext */
void encrypt_block(unsigned char * encrypted,
                  const unsigned char * plaintext,
                  const int accumulator,
                  BIGNUM * exponent,
                  BIGNUM * modulus,
                  BN_CTX * ctx)
{
    int i, tmp;
    int acc;
    unsigned char buf[ENCRYPTED_BLOCK_SIZE];
    unsigned char * p = buf;
    BIGNUM * result = BN_new();
    BIGNUM * block;

    /* clear out the temporary buffer */
    memset(buf, 0, ENCRYPTED_BLOCK_SIZE);

    /* clear out the result buffer */
    memset(encrypted, 0, ENCRYPTED_BLOCK_SIZE);

    /* pad/encode the plaintext out to ENCRYPTED_BLOCK_SIZE */
    *p = 0x15;
    p++;
    for(i = PLAINTEXT_BLOCK_SIZE - 1; i > 0; i--)
    {
        if(plaintext[i] < plaintext[i - 1])
        {
            tmp = (0x100 + plaintext[i]) - plaintext[i - 1];
            *p = (unsigned char)(tmp & 0xFF);
        }
        else
        {
            *p = plaintext[i] - plaintext[i - 1];
        }

        p++;
    }

    /* calculate last byte */
    if(plaintext[0] < accumulator)
    {
        tmp = (0x100 + plaintext[0]) - accumulator;
        *p = (unsigned char)(tmp & 0xff);
    }
    else
    {
        (*p) = plaintext[0] - accumulator;
    }

    /* memcpy(encrypted, buf, ENCRYPTED_BLOCK_SIZE); */

    printf("buf:\n");
    print_data(buf, 51);

    /* load the encoded plaintext */
    block = BN_bin2bn(buf, ENCRYPTED_BLOCK_SIZE, 0);
    
    /* do the RSA step */
    BN_mod_exp(result, block, exponent, modulus, ctx);

    /* clear out temporary buffer */
    memset(buf, 0, ENCRYPTED_BLOCK_SIZE);

    /* get the encrypted data out */
    BN_bn2bin(result, buf);

    printf("enc:\n");
    print_data(buf, 51);

    /* reverse the data as we copy it into the encrypted frame */
    for(i = 0; i < ENCRYPTED_BLOCK_SIZE; i++)
    {
        encrypted[i] = buf[(ENCRYPTED_BLOCK_SIZE - 1) - i];
    }

    /* free the result */
    BN_free(result);
}


/* This function encodes and encrypts a single block of plaintext data. */
void encrypt_frame(encrypted_frame_t * encrypted,
                   plaintext_frame_t * plaintext,
                   const unsigned char * private_exp,
                   const unsigned char * public_mod)
{
    int i;
    int accumulator;

    /* set up the bignum variables */
    BIGNUM *exponent = BN_bin2bn(private_exp, LYNX_RSA_KEY_SIZE, 0);
    BIGNUM *modulus = BN_bin2bn(public_mod, LYNX_RSA_KEY_SIZE, 0);
    BN_CTX *ctx = BN_CTX_new();

    /* pad and encrypt the blocks in the frame */
    for(i = plaintext->blocks - 1; i >= 0; i--)
    {
        if(i > 0)
            accumulator = plaintext->data[(i * PLAINTEXT_BLOCK_SIZE) - 1];
        else
            accumulator = 0;

        /* encrypte the block */
        encrypt_block(&encrypted->data[i * ENCRYPTED_BLOCK_SIZE], 
                      &plaintext->data[i * PLAINTEXT_BLOCK_SIZE], 
                      accumulator, exponent, modulus, ctx);

        /* store the block count */
        encrypted->blocks++;
    }

    /* free the bignum variables */
    BN_free(modulus);
    BN_free(exponent);
    BN_CTX_free(ctx);
}


/* This function loads an entire plaintext frame which is just 256 bytes */
int read_plaintext_frame(FILE * const in,
                         plaintext_frame_t * frame)
{
    int i, j;

    /* clear out the frame struct */
    memset(frame, 0, sizeof(plaintext_frame_t));

    /* read the block count */
    return (fread(frame->data, 1, MAX_PLAINTEXT_FRAME_SIZE, in) > 0);
}


int process_frame(FILE * in, FILE * out, frame_def_t * frame)
{
    int detected_blocks = 0;
    unsigned char tmp = 0;
    plaintext_frame_t plaintext_frame;
    encrypted_frame_t encrypted_frame;
 
    /* clear out the buffers */
    memset(&encrypted_frame, 0, sizeof(encrypted_frame_t));

    /* seek to the specified offset */
    if(fseek(in, frame->offset, SEEK_SET) != 0)
    {
        fprintf(stderr, "error: invalid frame offset %li\n", frame->offset);
        return 0;
    }

    /* read in the next encrypted frame of data */
    if(!read_plaintext_frame(in, &plaintext_frame))
    {
        fprintf(stderr, "error: failed to read plaintext block\n");
        return 0;
    }

    /* store the number of blocks to encrypt */
    plaintext_frame.blocks = frame->blocks;

    /* encrypt a single frame of the encrypted loader */
    printf("Encrypting %d blocks of plaintext from offset 0x%08x\n", plaintext_frame.blocks, (unsigned int)frame->offset);
    encrypt_frame(&encrypted_frame, &plaintext_frame, lynx_private_exp, lynx_public_mod);
 
    /* write the encrypted frame block count */
    tmp = 256 - encrypted_frame.blocks;
    fwrite(&tmp, sizeof(unsigned char), 1, out);

    /* write the encrypted frame of data */
    fwrite(encrypted_frame.data, (encrypted_frame.blocks * ENCRYPTED_BLOCK_SIZE), 1, out);

    return 1;
}

int read_frame_config(FILE * cfg, frame_def_t * frame, int line)
{
    long offset = 0;
    int blocks = 0;
    int state = 0;
    int started = 0;
    char c;

    while(fread(&c, 1, 1, cfg) == 1)
    {
        switch(state)
        {
            case 0:
            {
                /* we're reading the offset */
                if(isdigit(c))
                {
                    if(!started)
                        started = 1;

                    offset *= 10;
                    offset += (c - '0');
                }
                else if(c == '\n')
                {
                    if(started)
                    {
                        /* error, looking for a comma */
                        fprintf(stderr, 
                            "syntax error: expecting ',' after offset on line %d\n", line);
                    }
                    return 0;
                }
                else if(c == ',')
                {
                    /* switch to blocks state */
                    started = 0;
                    state = 1;
                }
                else
                {
                    fprintf(stderr,
                            "syntax error: expecting offset number on line %d\n", line);
                    return 0;
                }
                break;
            }
            case 1:
            {
                /* we're reading the blocks number */
                if((c == ' ') || (c == '\t'))
                {
                    /* eat whitespace */
                }
                else if(isdigit(c))
                {
                    blocks *= 10;
                    blocks += (c - '0');
                }
                else if(c == '\n')
                {
                    frame->offset = offset;
                    frame->blocks = blocks;
                    return 1;
                }
                else
                {
                    fprintf(stderr,
                            "syntax error: expecting block number on line %d\n", line);
                    return 0;
                }
                break;
            }
        }
    }

    fprintf(stderr, 
            "syntax error: reached EOF with incomplete frame definition, line %d\n", line);
    return 0;
}

int read_config_file(FILE * cfg, frame_def_t ** frames)
{
    int line = 1;
    int frame_count = 0;
    frame_def_t frame_def;

    memset(&frame_def, 0, sizeof(frame_def_t));

    while(read_frame_config(cfg, &frame_def, line))
    {
        /* make room for this new frame */
        (*frames) = realloc((*frames), (frame_count + 1) * sizeof(frame_def_t));

        /* copy the new frame into place */
        memcpy(&((*frames)[frame_count]), &frame_def, sizeof(frame_def_t));

        /* increment the line number */
        line++;

        /* increment the total frame count */
        frame_count++;
    }

    return frame_count;
}

void print_help(char * name)
{
    printf("usage: %s -c <config file> -p <plaintext binary> -e <encrypted binary>\n\n", name);
}

int main (int argc, char ** argv) 
{
    FILE *in;
    FILE *out;
    FILE *cfg;
    int i;
    int opt;
    int status;
    int frame_count = 0;
    char * cfg_file = 0;
    char * plaintext_file = 0;
    char * encrypted_file = 0;
    frame_def_t * frames = 0;

    if(argc < 3)
    {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }

    /* parse the command line options */
    while((opt = getopt(argc, argv, "hc:p:e:")) != -1) 
    {
        switch(opt) 
        {
            case 'c':
                cfg_file = strdup(optarg);
                break;
            case 'p':
                plaintext_file = strdup(optarg);
                break;
            case 'e':
                encrypted_file = strdup(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                status = EXIT_SUCCESS;
                goto cleanup;
            case ':':
                fprintf(stderr, "error: option `%c' needs a value\n\n", optopt);
                status = EXIT_FAILURE;
                goto cleanup;
            case '?':
                fprintf(stderr, "error: no such option: `%c'\n\n", optopt);
                status = EXIT_FAILURE;
                goto cleanup;
        }
    }

    if(!cfg_file || !plaintext_file || !encrypted_file)
    {
        print_help(argv[0]);
        status = EXIT_FAILURE;
        goto cleanup;
    }

    /* open the files */
    in = fopen(plaintext_file, "rb");
    out = fopen(encrypted_file, "wb+");
    cfg = fopen(cfg_file, "r");

    /* check for successful opens */
    if(!in)
    {
        fprintf(stderr, "failed to open plaintext loader file: %s\n\n", plaintext_file);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    if(!out)
    {
        fprintf(stderr, "failed to open encrypted loader file for writing: %s\n\n", encrypted_file);
        status = EXIT_FAILURE;
        goto cleanup;
    }
    if(!cfg)
    {
        fprintf(stderr, "failed to open config file: %s\n\n", cfg_file);
        status = EXIT_FAILURE;
        goto cleanup;
    }

    /* read in the frames config file */
    if((frame_count = read_config_file(cfg, &frames)) <= 0)
    {
        fprintf(stderr, "failed to read config file\n\n");
        status = EXIT_FAILURE;
        goto cleanup;
    }

    /* process the frames */
    for(i = 0; i < frame_count; i++)
    {
        /* process the first frame of plaintext data */
        if(!process_frame(in, out, &frames[i]))
        {
            fprintf(stderr, "failed to process frame %d\n\n", i);
            status = EXIT_FAILURE;
            goto cleanup;
        }
    }

    status = EXIT_SUCCESS;

cleanup:
    fclose(in);
    fclose(out);
    fclose(cfg);
    if(plaintext_file)
        free(plaintext_file);
    if(encrypted_file)
        free(encrypted_file);
    if(cfg_file)
        free(cfg_file);
    return status;
}

