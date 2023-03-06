SECTIONS
{
	__llvm_prf_data : AT(ADDR(__llvm_prf_data)) {
		__llvm_prf_start = .;
		__llvm_prf_data_start = .;
		*(__llvm_prf_data)
		__llvm_prf_data_end = .;
	}
	__llvm_prf_cnts : AT(ADDR(__llvm_prf_cnts)) {
		__llvm_prf_cnts_start = .;
		*(__llvm_prf_cnts)
		__llvm_prf_cnts_end = .;
	}
	__llvm_prf_names : AT(ADDR(__llvm_prf_names)) {
		__llvm_prf_names_start = .;
		*(__llvm_prf_names)
		__llvm_prf_names_end = .;
	}
	__llvm_prf_vals : AT(ADDR(__llvm_prf_vals)) {
		__llvm_prf_vals_start = .;
		*(__llvm_prf_vals)
		__llvm_prf_vals_end = .;
	}
	__llvm_prf_vnds : AT(ADDR(__llvm_prf_vnds)) {
		__llvm_prf_vnds_start = .;
		*(__llvm_prf_vnds)
		__llvm_prf_vnds_end = .;
		__llvm_prf_end = .;
	}
}
INSERT AFTER .data;