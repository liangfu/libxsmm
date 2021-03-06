/******************************************************************************
** Copyright (c) 2015-2017, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/

#include "generator_spgemm_csr_bsparse_soa.h"
#include "generator_gemm_common.h"
#include "generator_x86_instructions.h"
#include "generator_common.h"
#include <libxsmm_macros.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

LIBXSMM_INTERNAL_API_DEFINITION
void libxsmm_generator_spgemm_csr_bsparse_soa( libxsmm_generated_code*         io_generated_code,
                                               const libxsmm_gemm_descriptor*  i_xgemm_desc,
                                               const char*                     i_arch,
                                               const unsigned int*             i_row_idx,
                                               const unsigned int*             i_column_idx,
                                               const void*                     i_values ) {
  if ( strcmp(i_arch, "knl") == 0 ||
       strcmp(i_arch, "knm") == 0 ||
       strcmp(i_arch, "skx") == 0 ||
       strcmp(i_arch, "hsw") == 0 ||
       strcmp(i_arch, "snb") == 0 ) {
    libxsmm_generator_spgemm_csr_bsparse_soa_avx256_512( io_generated_code,
                                                         i_xgemm_desc,
                                                         i_arch,
                                                         i_row_idx,
                                                         i_column_idx,
                                                         i_values );
  } else {
    fprintf( stderr, "CSR + SOA is only available for AVX/AVX2/AVX512 at this point\n" );
    exit(-1);
  }
}

LIBXSMM_INTERNAL_API_DEFINITION
void libxsmm_generator_spgemm_csr_bsparse_soa_avx256_512( libxsmm_generated_code*         io_generated_code,
                                                          const libxsmm_gemm_descriptor*  i_xgemm_desc,
                                                          const char*                     i_arch,
                                                          const unsigned int*             i_row_idx,
                                                          const unsigned int*             i_column_idx,
                                                          const void*                     i_values ) {
  unsigned int l_m;
  unsigned int l_n;
  unsigned int l_k;
  unsigned int l_z;
  unsigned int l_row_elements;
  unsigned int l_soa_width;
  unsigned int l_max_cols = 0;
  unsigned int l_n_processed = 0;
  unsigned int l_n_limit = 0;
  unsigned int l_n_chunks = 0;
  unsigned int l_n_chunksize = 0;
  unsigned int l_found_mul = 0;
  unsigned int l_max_reg_block = 0;

  unsigned int l_nnz;
  /* cacheblocking for B */
  unsigned int l_max_rows = 0;
  unsigned int l_k_chunks;
  unsigned int l_k_chunksize;
  unsigned int l_k_processed;
  unsigned int l_k_limit;
  /* unrolling for A */
  unsigned int l_m_unroll_num;
  unsigned int l_m_processed;
  unsigned int l_m_limit;
  unsigned int l_row_reg_block;
  unsigned int l_m_r;
  unsigned int l_m_peeling;

  libxsmm_micro_kernel_config l_micro_kernel_config = { 0 };
  libxsmm_loop_label_tracker l_loop_label_tracker;
  libxsmm_gp_reg_mapping l_gp_reg_mapping;

  LIBXSMM_UNUSED(i_values);

  /* select soa width */
  if ( LIBXSMM_GEMM_PRECISION_F64 == i_xgemm_desc->datatype ) {
    if ( strcmp(i_arch, "knl") == 0 ||
         strcmp(i_arch, "knm") == 0 ||
         strcmp(i_arch, "skx") == 0 ) {
      l_soa_width = 8;
      l_max_reg_block = 28;
    } else {
      l_soa_width = 4;
      l_max_reg_block = 14;
    }
  } else {
    if ( strcmp(i_arch, "knl") == 0 ||
         strcmp(i_arch, "knm") == 0 ||
         strcmp(i_arch, "skx") == 0 ) {
      l_soa_width = 16;
      l_max_reg_block = 28;
    } else {
      l_soa_width = 8;
      l_max_reg_block = 14;
    }
  }

  /* define gp register mapping */
  libxsmm_reset_x86_gp_reg_mapping( &l_gp_reg_mapping );
  /* matching calling convention on Linux */
#if defined(_WIN32) || defined(__CYGWIN__)
  l_gp_reg_mapping.gp_reg_a = LIBXSMM_X86_GP_REG_RCX;
  l_gp_reg_mapping.gp_reg_b = LIBXSMM_X86_GP_REG_RDX;
  l_gp_reg_mapping.gp_reg_c = LIBXSMM_X86_GP_REG_R8;
  /* TODO: full support for Windows calling convention */
  l_gp_reg_mapping.gp_reg_a_prefetch = LIBXSMM_X86_GP_REG_RDI;
  l_gp_reg_mapping.gp_reg_b_prefetch = LIBXSMM_X86_GP_REG_RSI;
#else /* match calling convention on Linux */
  l_gp_reg_mapping.gp_reg_a = LIBXSMM_X86_GP_REG_RDI;
  l_gp_reg_mapping.gp_reg_b = LIBXSMM_X86_GP_REG_RSI;
  l_gp_reg_mapping.gp_reg_c = LIBXSMM_X86_GP_REG_RDX;
  l_gp_reg_mapping.gp_reg_a_prefetch = LIBXSMM_X86_GP_REG_RCX;
  l_gp_reg_mapping.gp_reg_b_prefetch = LIBXSMM_X86_GP_REG_R8;
#endif
  l_gp_reg_mapping.gp_reg_c_prefetch = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_mloop = LIBXSMM_X86_GP_REG_R12;
  l_gp_reg_mapping.gp_reg_nloop = LIBXSMM_X86_GP_REG_R13;
  l_gp_reg_mapping.gp_reg_kloop = LIBXSMM_X86_GP_REG_R14;
  l_gp_reg_mapping.gp_reg_help_0 = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_help_1 = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_help_2 = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_help_3 = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_help_4 = LIBXSMM_X86_GP_REG_UNDEF;
  l_gp_reg_mapping.gp_reg_help_5 = LIBXSMM_X86_GP_REG_UNDEF;

  /* define the micro kernel code gen properties */
  libxsmm_generator_gemm_init_micro_kernel_config_fullvector( &l_micro_kernel_config, i_xgemm_desc, i_arch, 0 );

  /* get max column in C */
  for ( l_m = 0; l_m < i_row_idx[i_xgemm_desc->k]; l_m++ ) {
    if (l_max_cols < i_column_idx[l_m]) {
      l_max_cols = i_column_idx[l_m];
    }
  }
  l_max_cols++;

  /* get number of non-zeros and max row in B */
  l_nnz = 0;
  l_max_rows = 0;
  for ( l_k = 0; l_k < (unsigned int)i_xgemm_desc->k; l_k++ ) {
    l_nnz += (i_row_idx[l_k+1] - i_row_idx[l_k]);
    if (i_row_idx[l_k+1] > i_row_idx[l_k])
      l_max_rows = l_k;
  }
  l_max_rows++;

  /* cacheblocking strategy for B */
  if ( LIBXSMM_GEMM_PRECISION_F64 == i_xgemm_desc->datatype ) {
    if ( (28*1024/8 - l_soa_width*(l_max_cols+i_xgemm_desc->lda)) > 0 )
      l_k_chunks = l_nnz / (28*1024/8 - l_soa_width*(l_max_cols+i_xgemm_desc->lda)) + 1;
    else
      l_k_chunks = 1;
  } else {
    if ( (28*1024/4 - l_soa_width*(l_max_cols+i_xgemm_desc->lda)) > 0 )
      l_k_chunks = l_nnz / (28*1024/4 - l_soa_width*(l_max_cols+i_xgemm_desc->lda)) + 1;
    else
      l_k_chunks = 1;
  }
  l_k_chunksize = ( (l_max_rows % l_k_chunks) == 0 ) ? (l_max_rows / l_k_chunks) : (l_max_rows / l_k_chunks) + 1;

  /* unroll strategy */
  l_m_unroll_num = 1;
  l_m_peeling = 0;
  if ( strcmp(i_arch, "knl") == 0 ||
       strcmp(i_arch, "knm") == 0 ||
       strcmp(i_arch, "skx") == 0 ) {
    if (l_nnz <= 150) {
      if ((unsigned int)i_xgemm_desc->m % 3 == 0) {
        l_m_unroll_num = 3;
        if (l_max_reg_block > 29) l_max_reg_block = 29;
      } else if ((unsigned int)i_xgemm_desc->m % 2 == 0) {
        l_m_unroll_num = 2;
        if (l_max_reg_block > 30) l_max_reg_block = 30;
      }
      else if ((unsigned int)i_xgemm_desc->m > 1) {
        l_m_unroll_num = 2;
        l_m_peeling = 1;
        if (l_max_reg_block > 30) l_max_reg_block = 30;
      }
    }
  } /* unroll strategy for other arch needs testing */

  /* open asm */
  libxsmm_x86_instruction_open_stream( io_generated_code, &l_gp_reg_mapping, i_arch, i_xgemm_desc->prefetch );

  /* loop over k-blocks */
  l_k_processed = 0;
  l_k_limit = l_k_chunksize;
  while ( l_k_processed < l_max_rows ) {

    /* define loop_label_tracker */
    libxsmm_reset_loop_label_tracker( &l_loop_label_tracker );

    /* loop over m-blocks : (1) unrolling block; (2) peeling block */
    l_m_processed = 0;
    l_m_limit = (unsigned int)i_xgemm_desc->m - l_m_peeling;
    while ( l_m_processed < (unsigned int)i_xgemm_desc->m ) {

      /* check for unrolling for peeling */
      if (l_m_processed == 0) { /* unrolling */
        /* open m loop */
        libxsmm_x86_instruction_register_jump_label( io_generated_code, &l_loop_label_tracker );
        libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_add_instruction, l_gp_reg_mapping.gp_reg_mloop, l_m_unroll_num );
      } else { /* peeling */
        l_m_unroll_num = 1;
      }
      l_row_reg_block = l_max_reg_block / l_m_unroll_num;

      /* calculate the chunk size of current columns to work on */
      l_n_chunks = ( (l_max_cols % l_row_reg_block) == 0 ) ? (l_max_cols / l_row_reg_block) : (l_max_cols / l_row_reg_block) + 1;
      l_n_chunksize = ( (l_max_cols % l_n_chunks) == 0 ) ? (l_max_cols / l_n_chunks) : (l_max_cols / l_n_chunks) + 1;

      /* loop over n-blocks */
      l_n_processed = 0;
      l_n_limit = l_n_chunksize;
      while ( l_n_processed < l_max_cols ) {
#if 0
        printf("l_max_cols: %i, l_n_processed: %i, l_n_limit: %i\n", l_max_cols, l_n_processed, l_n_limit);
#endif

        /* load C accumulator */
        for ( l_m_r = 0; l_m_r < l_m_unroll_num; l_m_r++) {
          for ( l_n = 0; l_n < l_n_limit - l_n_processed; l_n++ ) {
            if ( i_xgemm_desc->beta == 0 ) {
              libxsmm_x86_instruction_vec_compute_reg( io_generated_code,
                                                       l_micro_kernel_config.instruction_set,
                                                       l_micro_kernel_config.vxor_instruction,
                                                       l_micro_kernel_config.vector_name,
                                                       l_n + l_row_reg_block*l_m_r, l_n + l_row_reg_block*l_m_r, l_n + l_row_reg_block*l_m_r );
            } else {
              libxsmm_x86_instruction_vec_move( io_generated_code,
                                                l_micro_kernel_config.instruction_set,
                                                l_micro_kernel_config.c_vmove_instruction,
                                                l_gp_reg_mapping.gp_reg_c,
                                                LIBXSMM_X86_GP_REG_UNDEF, 0,
                                                (l_n_processed + l_n + i_xgemm_desc->ldc*l_m_r)*l_soa_width*l_micro_kernel_config.datatype_size,
                                                l_micro_kernel_config.vector_name,
                                                l_n + l_row_reg_block*l_m_r, 0, 0 );
            }
          }
        }

        /* do dense soa times sparse multiplication */
        /* for ( l_k = 0; l_k < (unsigned int)i_xgemm_desc->k; l_k++ ) { */
        for ( l_k = l_k_processed; l_k < l_k_limit; l_k++ ) {
          l_row_elements = i_row_idx[l_k+1] - i_row_idx[l_k];
          l_found_mul = 0;
          /* check if we actually need to multiply */
          for ( l_z = 0; l_z < l_row_elements; l_z++ ) {
            if ( (i_column_idx[i_row_idx[l_k] + l_z] < (unsigned int)i_xgemm_desc->n) &&
                 (i_column_idx[i_row_idx[l_k] + l_z] >= l_n_processed)                &&
                 (i_column_idx[i_row_idx[l_k] + l_z] < l_n_limit) )                        {
              l_found_mul = 1;
            }
          }
          /* only load A if multiplication loop is not empty */
          if (l_found_mul != 0) {
            for ( l_m_r = 0; l_m_r < l_m_unroll_num; l_m_r++) {
              libxsmm_x86_instruction_vec_move( io_generated_code,
                                                l_micro_kernel_config.instruction_set,
                                                l_micro_kernel_config.a_vmove_instruction,
                                                l_gp_reg_mapping.gp_reg_a,
                                                LIBXSMM_X86_GP_REG_UNDEF, 0,
                                                (l_k+i_xgemm_desc->lda*l_m_r)*l_soa_width*l_micro_kernel_config.datatype_size,
                                                l_micro_kernel_config.vector_name,
                                                /*28+(l_k%4), 0, 0 );*/
                                                l_max_reg_block+l_m_r, 0, 0 );
            }
          }
          /* loop over element in the row of B and multiply*/
          for ( l_z = 0; l_z < l_row_elements; l_z++ ) {
            /* check k such that we just use columns which actually need to be multiplied */
            if ( (i_column_idx[i_row_idx[l_k] + l_z] < (unsigned int)i_xgemm_desc->n) &&
                 (i_column_idx[i_row_idx[l_k] + l_z] >= l_n_processed)                &&
                 (i_column_idx[i_row_idx[l_k] + l_z] < l_n_limit) )                        {
              if ( strcmp(i_arch, "knl") == 0 ||
                   strcmp(i_arch, "knm") == 0 ||
                   strcmp(i_arch, "skx") == 0 ) {
                for ( l_m_r = 0; l_m_r < l_m_unroll_num; l_m_r++) {
                  libxsmm_x86_instruction_vec_compute_mem( io_generated_code,
                                                           l_micro_kernel_config.instruction_set,
                                                           l_micro_kernel_config.vmul_instruction,
                                                           1,
                                                           l_gp_reg_mapping.gp_reg_b,
                                                           LIBXSMM_X86_GP_REG_UNDEF,
                                                           0,
                                                           (i_row_idx[l_k] + l_z) * l_micro_kernel_config.datatype_size,
                                                           l_micro_kernel_config.vector_name,
                                                           l_max_reg_block+l_m_r, /*28+(l_k%4),*/
                                                           i_column_idx[i_row_idx[l_k] + l_z] - l_n_processed + l_row_reg_block*l_m_r );
                }
              } else if ( strcmp(i_arch, "hsw") == 0 ) {
                libxsmm_x86_instruction_vec_move( io_generated_code,
                                                  l_micro_kernel_config.instruction_set,
                                                  l_micro_kernel_config.b_vmove_instruction,
                                                  l_gp_reg_mapping.gp_reg_b,
                                                  LIBXSMM_X86_GP_REG_UNDEF, 0,
                                                  (i_row_idx[l_k] + l_z) * l_micro_kernel_config.datatype_size,
                                                  l_micro_kernel_config.vector_name,
                                                  15, 0, 0 );
                libxsmm_x86_instruction_vec_compute_reg( io_generated_code,
                                                         l_micro_kernel_config.instruction_set,
                                                         l_micro_kernel_config.vmul_instruction,
                                                         l_micro_kernel_config.vector_name,
                                                         l_max_reg_block,
                                                         15,
                                                         i_column_idx[i_row_idx[l_k] + l_z] - l_n_processed );
              } else {
                libxsmm_x86_instruction_vec_move( io_generated_code,
                                                  l_micro_kernel_config.instruction_set,
                                                  l_micro_kernel_config.b_vmove_instruction,
                                                  l_gp_reg_mapping.gp_reg_b,
                                                  LIBXSMM_X86_GP_REG_UNDEF, 0,
                                                  (i_row_idx[l_k] + l_z) * l_micro_kernel_config.datatype_size,
                                                  l_micro_kernel_config.vector_name,
                                                  15, 0, 0 );
                libxsmm_x86_instruction_vec_compute_reg( io_generated_code,
                                                         l_micro_kernel_config.instruction_set,
                                                         l_micro_kernel_config.vmul_instruction,
                                                         l_micro_kernel_config.vector_name,
                                                         l_max_reg_block,
                                                         15,
                                                         15 );
                libxsmm_x86_instruction_vec_compute_reg( io_generated_code,
                                                         l_micro_kernel_config.instruction_set,
                                                         l_micro_kernel_config.vadd_instruction,
                                                         l_micro_kernel_config.vector_name,
                                                         15,
                                                         i_column_idx[i_row_idx[l_k] + l_z] - l_n_processed,
                                                         i_column_idx[i_row_idx[l_k] + l_z] - l_n_processed );
              }
            }
          }
        }

        /* store C accumulator */
        for ( l_m_r = 0; l_m_r < l_m_unroll_num; l_m_r++) {
          for ( l_n = 0; l_n < l_n_limit - l_n_processed; l_n++ ) {
            libxsmm_x86_instruction_vec_move( io_generated_code,
                                              l_micro_kernel_config.instruction_set,
                                              l_micro_kernel_config.c_vmove_instruction,
                                              l_gp_reg_mapping.gp_reg_c,
                                              LIBXSMM_X86_GP_REG_UNDEF, 0,
                                              (l_n_processed + l_n + i_xgemm_desc->ldc*l_m_r)*l_soa_width*l_micro_kernel_config.datatype_size,
                                              l_micro_kernel_config.vector_name,
                                              l_n + l_row_reg_block*l_m_r, 0, 1 );
          }
        }

        /* adjust n progression */
        l_n_processed += l_n_chunksize;
        l_n_limit = LIBXSMM_MIN(l_n_processed + l_n_chunksize, l_max_cols);
      }

      /* advance C pointer */
      libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_add_instruction, l_gp_reg_mapping.gp_reg_c,
                                         l_micro_kernel_config.datatype_size*l_soa_width*i_xgemm_desc->ldc*l_m_unroll_num);

      /* advance A pointer */
      libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_add_instruction, l_gp_reg_mapping.gp_reg_a,
                                       l_micro_kernel_config.datatype_size*l_soa_width*i_xgemm_desc->lda*l_m_unroll_num);

      if (l_m_processed == 0) {
        /* close m loop */
        libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_cmp_instruction, l_gp_reg_mapping.gp_reg_mloop, l_m_limit );
        libxsmm_x86_instruction_jump_back_to_label( io_generated_code, l_micro_kernel_config.alu_jmp_instruction, &l_loop_label_tracker );
      }

      /* adjust m progression : switch to the peeling part */
      l_m_processed = l_m_limit;
      l_m_limit = i_xgemm_desc->m;
    }

    if (l_k_limit < l_max_rows) {
      /* reset C pointer */
      libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_sub_instruction, l_gp_reg_mapping.gp_reg_c,
                                         l_micro_kernel_config.datatype_size*l_soa_width*i_xgemm_desc->ldc*i_xgemm_desc->m);

      /* reset A pointer */
      libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_sub_instruction, l_gp_reg_mapping.gp_reg_a,
                                       l_micro_kernel_config.datatype_size*l_soa_width*i_xgemm_desc->lda*i_xgemm_desc->m);

      /* reset m loop */
      libxsmm_x86_instruction_alu_imm( io_generated_code, l_micro_kernel_config.alu_mov_instruction, l_gp_reg_mapping.gp_reg_mloop, 0 );
    }

    /* adjust k progression */
    l_k_processed += l_k_chunksize;
    l_k_limit = LIBXSMM_MIN(l_k_processed + l_k_chunksize, l_max_rows);
  }

  /* close asm */
  libxsmm_x86_instruction_close_stream( io_generated_code, &l_gp_reg_mapping, i_arch, i_xgemm_desc->prefetch );
}

