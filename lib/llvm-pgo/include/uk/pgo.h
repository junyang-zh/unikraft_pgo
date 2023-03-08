#ifndef _SYS_PGO_H
#define _SYS_PGO_H

typedef struct llvm_perf_file {
	struct prf_private_data *private_data;
} llvm_perf_file;

int llvm_perf_open(struct llvm_perf_file *file);

ssize_t llvm_perf_read(struct llvm_perf_file *file, char *buf, size_t count, size_t off);

int llvm_perf_release(struct llvm_perf_file *file);

ssize_t llvm_perf_reset();

void *llvm_perf_dump_thread_fn(void *arg);

#endif // _SYS_PGO_H
