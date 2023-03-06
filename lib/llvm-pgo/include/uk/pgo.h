#ifndef _SYS_PGO_H
#define _SYS_PGO_H

typedef struct llvm_perf_file {
	struct prf_private_data *private_data;
} llvm_perf_file;

int llvm_perf_open(struct llvm_perf_file *file);

ssize_t llvm_perf_read(struct llvm_perf_file *file, char *buf, size_t count);

int llvm_perf_release(struct llvm_perf_file *file);

ssize_t llvm_perf_reset();

#endif // _SYS_PGO_H
