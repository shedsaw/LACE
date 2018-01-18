/*
 *  -- LACE (version 0.0) --
 *     Univ. of Tennessee, Knoxville
 *
 *     @author Stephen Wood
 *
 */
#include "../include/sparse.h"
#include <mkl.h>
#include <stdlib.h>
#include <stdio.h>

extern "C"
void
data_PariLU_v3_7(data_d_matrix * A, data_d_matrix * L, data_d_matrix * U, int tile)
{
  data_zdiameter(A);
  // printf("A:\n");
  // data_zwrite_csr( A );
  data_z_pad_csr(A, tile);
  // printf("A padded by %d tile:\n", tile);
  // data_zwrite_csr( A );

  std::vector<Int3> Ltiles;
  std::vector<Int3> Utiles;
  data_sparse_tilepatterns(tile, tile, &Ltiles, &Utiles, A);
  printf("\nL's active tiles:\n");
  for (int i = 0; i < int(Ltiles.size()); i++) {
    printf("[%d, %d : %d]\n", Ltiles[i].a[0], Ltiles[i].a[1], Ltiles[i].a[2]);
  }
  printf("\nU's active tiles:\n");
  for (int i = 0; i < int(Utiles.size()); i++) {
    printf("[%d, %d : %d]\n", Utiles[i].a[0], Utiles[i].a[1], Utiles[i].a[2]);
  }


  // Separate the lower, upper, and diagonal elements
  // into L, U, and D respectively.
  L->diagorder_type = Magma_UNITY;
  data_zmconvert(*A, L, Magma_CSR, Magma_CSRL);
  data_d_matrix Lc = { Magma_CSR };
  Lc.diagorder_type = Magma_UNITY;
  data_zmconvert(*A, &Lc, Magma_CSR, Magma_CSRL);
  // printf("L done.\n");
  // data_zwrite_csr( L );

  U->diagorder_type = Magma_VALUE;
  U->major = MagmaRowMajor;
  data_zmconvert(*A, U, Magma_CSR, Magma_CSRU);
  data_d_matrix Uc = { Magma_CSR };
  Uc.diagorder_type = Magma_VALUE;
  data_zmconvert(*A, &Uc, Magma_CSR, Magma_CSRU);
  // printf("U done.\n");
  // data_zwrite_csr( U );

  data_d_matrix D = { Magma_DENSED };
  data_zmconvert(*A, &D, Magma_CSR, Magma_DENSED);
  // data_zprint_dense( D );
  data_d_matrix Dc = { Magma_DENSED };
  data_zmconvert(*A, &Dc, Magma_CSR, Magma_DENSED);
  // printf("D done.\n");

  // Set diagonal elements to the recipricol
  #pragma omp parallel
  #pragma omp for nowait
  for (int i = 0; i < D.nnz; i++) {
    D.val[ i ] = 1.0 / D.val[ i ];
  }

  int row_limit = A->num_rows;
  if (A->pad_rows > 0 && A->pad_cols > 0) {
    row_limit = A->pad_rows;
  }

  // ParLU element wise
  int iter       = 0;
  dataType tmp   = 0.0;
  dataType step  = 1.0;
  dataType tol   = 1.0e-15;
  dataType Anorm = 0.0;

  int num_threads, nthreads1, nthreads2;
  #pragma omp parallel
  {
    num_threads = omp_get_num_threads();
  }
  nthreads1 = num_threads / 2;
  nthreads2 = num_threads - nthreads1;
  int Lchunk = int(Ltiles.size()) / ( 2 * nthreads1 );
  int Uchunk = int(Utiles.size()) / ( 2 * nthreads2 );

  dataType cone = 1.0;
  // dataType czero = 0.0;

  int ii, jj, ti, tj;
  int maxrow, maxcol;
  int span;
  int infol, infou, infospmm;
  dataType sum = 0.0;

  data_zfrobenius(*A, &Anorm);
  printf("%% Anorm = %e\n", Anorm);
  printf("%% A->diameter=%d\n", A->diameter);
  printf("%% A->num_rows=%d\n", A->num_rows);
  printf("%% A->num_cols=%d\n", A->num_cols);
  printf("%% A->pad_rows=%d\n", A->pad_rows);
  printf("%% A->pad_cols=%d\n", A->pad_cols);
  printf("%% A->nnz=%d\n", A->nnz);
  printf("%% A->true_nnz=%d\n", A->true_nnz);

  dataType wstart = omp_get_wtime();
  while (step > tol && iter < 10000) {
    step = 0.0;
    #pragma omp parallel private(ii, jj, ti, tj, maxrow, maxcol, tmp, span, sum, infol, infou, infospmm )
    {
      #pragma omp master
      {
        #pragma omp task
        #pragma \
          omp parallel private(ii, jj, ti, tj, maxrow, maxcol, tmp, span, sum, infol, infou, infospmm ) num_threads(nthreads1)
        {
          // #pragma omp for schedule(static,1) reduction(+:step) nowait
          // #pragma omp for reduction(+:step) nowait
          #pragma omp for schedule(static,Lchunk) reduction(+:step) nowait
          for (int i = 0; i < int(Utiles.size()); i++) {
            ti     = Utiles[i].a[0];
            tj     = Utiles[i].a[1];
            maxrow = ti + tile;
            maxcol = tj + tile;
            // int span = MIN( (ti+tile - 0), (tj+tile - 0) );
            span = MIN( (maxrow - Utiles[i].a[2]), (maxcol - Utiles[i].a[2]) );
            // printf("==== ti=%d tj=%d span=%d\n", ti, tj, span);

            data_d_matrix C = { Magma_CSR };
            data_sparse_subsparse_spmm(tile, span, ti, tj, &Utiles[i], L, U, &C);

            /*
             * data_d_matrix Lsub = {Magma_CSR};
             * infol = data_sparse_subsparse_cs_lowerupper( tile, span, ti, Utiles[i].a[2], L, &Lsub );
             *
             * data_d_matrix Usub = {Magma_CSR};
             * infou = data_sparse_subsparse_cs_lowerupper( span, tile, Utiles[i].a[2], tj, U, &Usub );
             *
             * if (infol != 0 || infou != 0) {
             * printf("==== ti=%d tj=%d span=%d\n", ti, tj, span);
             * printf("infol=%d infou=%d\n", infol, infou);
             * fflush(stdout);
             * exit(1);
             * }
             *
             * // caLculate update
             * data_d_matrix C = {Magma_CSR};
             * infospmm = data_z_spmm( cone, Lsub, Usub, &C );
             *
             * if ( infospmm != 0 ) {
             * //printf("***\t*** Empty C ***\t***\n");
             * C.num_rows = tile;
             * C.num_cols = 1;
             * C.nnz = 1;
             * C.true_nnz = 1;
             * C.row = (int*) calloc( (tile+1), sizeof(int) );
             * C.col = (int*) calloc( 1, sizeof(int) );
             * C.val = (dataType*) calloc( 1, sizeof(dataType) );
             * for (int ic=0; ic<tile+1; ic++) {
             *  C.row[ic] = 0;
             * }
             * C.col[0] = 0;
             * C.val[0] = 0.0;
             * }
             *
             * data_zmfree( &Lsub );
             * data_zmfree( &Usub );
             */


            // upper tiles and tiles along major diagonal
            if (ti <= tj) {
              // begin updating values of U
              for (int i = ti; i < maxrow; i++) {
                for (int j = Uc.row[i]; j < Uc.row[i + 1]; j++) {
                  if (U->col[j] >= tj && U->col[j] < maxcol && U->col[j] != i) {
                    ii = i - ti;
                    jj = U->col[j] - tj;

                    for (int k = C.row[ii]; k < C.row[ii + 1]; k++) {
                      if (C.col[k] == jj) {
                        tmp         = Uc.val[ j ] - C.val[ k ];
                        step       += pow(U->val[ j ] - tmp, 2);
                        U->val[ j ] = tmp;
                      }
                    }
                  }
                }
              }
              if (ti == tj) {
                for (int i = ti; i < maxrow; i++) {
                  for (int j = U->row[i]; j < U->row[i + 1]; j++) {
                    if (U->col[j] == i) {
                      ii = i - ti;
                      jj = U->col[j] - tj;

                      for (int k = C.row[ii]; k < C.row[ii + 1]; k++) {
                        if (C.col[k] == jj) {
                          tmp        = 1.0 / ( Dc.val[ i ] - C.val[ k ] );
                          step      += pow(D.val[ i ] - tmp, 2);
                          D.val[ i ] = tmp;
                        }
                      }
                    }
                  }
                }
                // begin updating values of L
                for (int i = ti; i < maxrow; i++) {
                  for (int j = Lc.row[i]; j < Lc.row[i + 1]; j++) {
                    if (L->col[j] >= tj && L->col[j] < maxcol && L->col[j] != i) {
                      ii  = i - ti;
                      jj  = L->col[j] - tj;
                      sum = 0.0;
                      for (int k = C.row[ii]; k < C.row[ii + 1]; k++) {
                        if (C.col[k] == jj) {
                          sum = C.val[ k ];
                        }
                      }
                      tmp         = ( (Lc.val[ j ] - sum) * D.val[ L->col[j] ] );
                      step       += pow(L->val[ j ] - tmp, 2);
                      L->val[ j ] = tmp;
                    }
                  }
                }
              }
              // done updating values for tiles in the upper triangle and on the major diagonal
            }
            // strictly lower tiles

            /*else {
             *
             * // begin updating values of L
             * for(int i=ti; i < maxrow; i++ ) {
             *  for(int j=Lc.row[i]; j < Lc.row[i+1]; j++ ) {
             *    if (Lc.col[j] >= tj && L->col[j] < maxcol  && L->col[j] != i) {
             *      ii = i - ti;
             *      jj = L->col[j] - tj;
             *
             *      sum = 0.0;
             *      for(int k=C.row[ii]; k < C.row[ii+1]; k++ ) {
             *        if ( C.col[k] == jj ) {
             *          sum = C.val[ k ];
             *        }
             *      }
             *      tmp = ( (Lc.val[ j ] - sum)*D.val[ L->col[j] ] );
             *      step += pow( L->val[ j ] - tmp, 2 );
             *      L->val[ j ] = tmp;
             *    }
             *  }
             * }
             * // done updating values for strictly lower tiles
             * }*/

            data_zmfree(&C);
          }
        }

        #pragma omp task
        #pragma \
          omp parallel private(ii, jj, ti, tj, maxrow, maxcol, tmp, span, sum, infol, infou, infospmm ) num_threads(nthreads2)
        {
          // #pragma omp for schedule(static,1) reduction(+:step) nowait
          // #pragma omp for reduction(+:step) nowait
          #pragma omp for schedule(static,Uchunk) reduction(+:step) nowait
          for (int i = 0; i < int(Ltiles.size()); i++) {
            ti     = Ltiles[i].a[0];
            tj     = Ltiles[i].a[1];
            maxrow = ti + tile;
            maxcol = tj + tile;
            // int span = MIN( (ti+tile - 0), (tj+tile - 0) );
            span = MIN( (maxrow - Ltiles[i].a[2]), (maxcol - Ltiles[i].a[2]) );
            // printf("==== ti=%d tj=%d span=%d\n", ti, tj, span);
            data_d_matrix Lsub = { Magma_CSR };
            infol = data_sparse_subsparse_cs_lowerupper(tile, span, ti, Ltiles[i].a[2], L, &Lsub);

            data_d_matrix Usub = { Magma_CSR };
            infou = data_sparse_subsparse_cs_lowerupper(span, tile, Ltiles[i].a[2], tj, U, &Usub);

            if (infol != 0 || infou != 0) {
              printf("==== ti=%d tj=%d span=%d\n", ti, tj, span);
              printf("infol=%d infou=%d\n", infol, infou);
              fflush(stdout);
              exit(1);
            }

            // caLculate update
            data_d_matrix C = { Magma_CSR };
            infospmm = data_z_spmm(cone, Lsub, Usub, &C);

            if (infospmm != 0) {
              // printf("***\t*** Empty C ***\t***\n");
              C.num_rows = tile;
              C.num_cols = 1;
              C.nnz      = 1;
              C.true_nnz = 1;
              C.row      = (int *) calloc( (tile + 1), sizeof(int) );
              C.col      = (int *) calloc(1, sizeof(int) );
              C.val      = (dataType *) calloc(1, sizeof(dataType) );
              // for (int ic=0; ic<tile+1; ic++) {
              //  C.row[ic] = 0;
              // }
              // C.col[0] = 0;
              // C.val[0] = 0.0;
            }

            data_zmfree(&Lsub);
            data_zmfree(&Usub);

            /*
             * // upper tiles and tiles along major diagonal
             * if (ti<=tj) {
             * // begin updating values of U
             * for(int i=ti; i < maxrow; i++ ) {
             *  for(int j=Uc.row[i]; j < Uc.row[i+1]; j++ ) {
             *    if (U->col[j] >= tj && U->col[j] < maxcol && U->col[j] != i) {
             *      ii = i - ti;
             *      jj = U->col[j] - tj;
             *
             *      for(int k=C.row[ii]; k < C.row[ii+1]; k++ ) {
             *        if ( C.col[k] == jj ) {
             *          tmp = Uc.val[ j ] - C.val[ k ];
             *          step += pow( U->val[ j ] - tmp, 2 );
             *          U->val[ j ] = tmp;
             *        }
             *      }
             *
             *    }
             *  }
             * }
             * if (ti==tj) {
             *  for(int i=ti; i < maxrow; i++ ) {
             *    for(int j=U->row[i]; j < U->row[i+1]; j++) {
             *      if ( U->col[j] == i ) {
             *        ii = i - ti;
             *        jj = U->col[j] - tj;
             *
             *        for(int k=C.row[ii]; k < C.row[ii+1]; k++ ) {
             *          if ( C.col[k] == jj ) {
             *            tmp = 1.0/( Dc.val[ i ] - C.val[ k ] );
             *            step += pow( D.val[ i ] - tmp, 2 );
             *            D.val[ i ] = tmp;
             *          }
             *        }
             *
             *      }
             *    }
             *  }
             *  // begin updating values of L
             *  for(int i=ti; i < maxrow; i++ ) {
             *    for(int j=Lc.row[i]; j < Lc.row[i+1]; j++ ) {
             *      if (L->col[j] >= tj && L->col[j] < maxcol  && L->col[j] != i) {
             *        ii = i - ti;
             *        jj = L->col[j] - tj;
             *        sum = 0.0;
             *        for(int k=C.row[ii]; k < C.row[ii+1]; k++ ) {
             *          if ( C.col[k] == jj ) {
             *            sum = C.val[ k ];
             *          }
             *        }
             *        tmp = ( (Lc.val[ j ] - sum)*D.val[ L->col[j] ] );
             *        step += pow( L->val[ j ] - tmp, 2 );
             *        L->val[ j ] = tmp;
             *
             *      }
             *    }
             *  }
             * }
             * // done updating values for tiles in the upper triangle and on the major diagonal
             * }
             * // strictly lower tiles
             * else {
             */
            // begin updating values of L
            for (int i = ti; i < maxrow; i++) {
              for (int j = Lc.row[i]; j < Lc.row[i + 1]; j++) {
                if (Lc.col[j] >= tj && L->col[j] < maxcol && L->col[j] != i) {
                  ii = i - ti;
                  jj = L->col[j] - tj;

                  sum = 0.0;
                  for (int k = C.row[ii]; k < C.row[ii + 1]; k++) {
                    if (C.col[k] == jj) {
                      sum = C.val[ k ];
                    }
                  }
                  tmp         = ( (Lc.val[ j ] - sum) * D.val[ L->col[j] ] );
                  step       += pow(L->val[ j ] - tmp, 2);
                  L->val[ j ] = tmp;
                }
              }
            }
            // done updating values for strictly lower tiles
            // }

            data_zmfree(&C);
          }
        }
      }
    }


    step /= Anorm;
    iter++;
    printf("%% iteration = %d step = %e\n", iter, step);
  }
  dataType wend     = omp_get_wtime();
  dataType ompwtime = (dataType) (wend - wstart) / ((dataType) iter);

  #pragma omp parallel
  {
    num_threads = omp_get_num_threads();
    #pragma omp for nowait
    for (int i = 0; i < row_limit; i++) {
      for (int j = U->row[i]; j < U->row[i + 1]; j++) {
        if (U->col[j] == i) {
          U->val[ j ] = 1.0 / D.val[ i ];
        }
      }
    }
  }


  printf(
    "%% PariLU v3.7 used %d OpenMP threads and required %d iterations, %f wall clock seconds, and an average of %f wall clock seconds per iteration as measured by omp_get_wtime()\n",
    num_threads, iter, wend - wstart, ompwtime);
  fflush(stdout);

  data_zmfree(&D);
  data_zmfree(&Dc);
  data_zmfree(&Lc);
  data_zmfree(&Uc);
} // data_PariLU_v3_7
