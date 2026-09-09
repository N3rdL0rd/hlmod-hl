#include "bytecode_loader.h"
#include <hlmod.h>
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>

char g_code_sha256[65] = {0};

hl_code *load_code( const pchar *file, char **error_msg, bool print_errors ) {
	hl_code *code;
	FILE *f = pfopen(file,"rb");
	int pos, size;
	char *fdata;
	if( f == NULL ) {
		if( print_errors ) pprintf("File not found '%s'\n",file);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = (int)ftell(f);
	fseek(f, 0, SEEK_SET);
	fdata = (char*)malloc(size);
	pos = 0;
	while( pos < size ) {
		int r = (int)fread(fdata + pos, 1, size-pos, f);
		if( r <= 0 ) {
			if( print_errors ) pprintf("Failed to read '%s'\n",file);
			return NULL;
		}
		pos += r;
	}
	fclose(f);
    SHA256_CTX ctx;
    SHA256_BYTE hash[SHA256_BLOCK_SIZE];

    sha256_init(&ctx);
    sha256_update(&ctx, (SHA256_BYTE*)fdata, size);
    sha256_final(&ctx, hash);

    for(int i = 0; i < SHA256_BLOCK_SIZE; i++)
    	sprintf(&g_code_sha256[i * 2], "%02x", hash[i]);

    printf("[hlmod] Bytecode SHA256: %s\n", g_code_sha256);

	code = hl_code_read((unsigned char*)fdata, size, error_msg);
	free(fdata);
	return code;
}
