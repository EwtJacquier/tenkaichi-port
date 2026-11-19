// prxdec: a PSP module (~PSP) decrypted with PPSSPP's PrxDecrypter, and gunzipped if it is compressed.
#include <cstdio>
#include <cstring>
#include <vector>
#include <zlib.h>
#include "Common/Common.h"
#include "Common/Swap.h"
#include "Core/ELF/PrxDecrypter.h"
int main(int argc, char **argv) {
	if (argc != 3) { fprintf(stderr, "usage: prxdec in.prx out.elf\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	std::vector<u8> in;
	u8 buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) in.insert(in.end(), buf, buf + n);
	fclose(f);
	if (in.size() < 0x150 || memcmp(in.data(), "~PSP", 4)) { fprintf(stderr, "not a ~PSP module\n"); return 1; }
	u32 elf_size, psp_size;
	u16 comp;
	memcpy(&comp, &in[6], 2);
	memcpy(&elf_size, &in[0x28], 4);
	memcpy(&psp_size, &in[0x2C], 4);
	std::vector<u8> out(in.size() + elf_size + 0x1000);
	int dec = pspDecryptPRX(in.data(), out.data(), psp_size, nullptr);
	if (dec <= 0) { fprintf(stderr, "decryption failed (%d)\n", dec); return 1; }
	std::vector<u8> elf;
	if (comp & 1) {
		elf.resize(elf_size + 0x10000);
		z_stream z = {};
		inflateInit2(&z, 16 + MAX_WBITS);
		z.next_in = out.data(), z.avail_in = (uInt)dec, z.next_out = elf.data(), z.avail_out = (uInt)elf.size();
		int r = inflate(&z, Z_FINISH);
		inflateEnd(&z);
		if (r != Z_STREAM_END) { fprintf(stderr, "gunzip failed (%d)\n", r); return 1; }
		elf.resize(z.total_out);
	} else elf.assign(out.begin(), out.begin() + dec);
	FILE *o = fopen(argv[2], "wb");
	fwrite(elf.data(), 1, elf.size(), o);
	fclose(o);
	printf("%s: %d bytes decrypted%s -> %zu bytes\n", argv[1], dec, comp & 1 ? ", gunzipped" : "", elf.size());
	return 0;
}
