/*
 * Link stub for the console's libSceVideodec2: the names the title imports
 * (ps5/modules/ps5vdec.c). Only linked against, never run: the console's own
 * library answers these calls.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
int sceVideodec2QueryComputeMemoryInfo(void *info) { (void)info; return -1; }
int sceVideodec2AllocateComputeQueue(const void *config, const void *memory, void **queue) { (void)config; (void)memory; (void)queue; return -1; }
int sceVideodec2ReleaseComputeQueue(void *queue) { (void)queue; return -1; }
int sceVideodec2QueryDecoderMemoryInfo(const void *config, void *memory) { (void)config; (void)memory; return -1; }
int sceVideodec2CreateDecoder(const void *config, const void *memory, void **decoder) { (void)config; (void)memory; (void)decoder; return -1; }
int sceVideodec2DeleteDecoder(void *decoder) { (void)decoder; return -1; }
int sceVideodec2Decode(void *decoder, const void *input, void *frame, void *output) { (void)decoder; (void)input; (void)frame; (void)output; return -1; }
int sceVideodec2Flush(void *decoder, void *frame, void *output) { (void)decoder; (void)frame; (void)output; return -1; }
int sceVideodec2Reset(void *decoder) { (void)decoder; return -1; }
int sceVideodec2GetPictureInfo(const void *output, void *first, void *second) { (void)output; (void)first; (void)second; return -1; }
