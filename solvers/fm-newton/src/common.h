#pragma GCC diagnostic ignored "-Wunused-result"

#ifndef _COMMON_H_
#define _COMMON_H_

#define flag { printf("\nLINE: %d\n", __LINE__); fflush(stdout); }

#include <cstdio>
#include <string>
#include <vector>
#include <cmath>

#include <pmmintrin.h>

struct Node
{
    Node(size_t const f, size_t const j, double const v) : f(f), j(j), v(v) {}
    size_t f, j;
    double v;
};

struct SpMat
{
    SpMat() : nr_feature(0), nr_instance(0) {}
    std::vector<size_t> P;
    std::vector<Node> X;
    std::vector<double> Y;
    size_t nr_feature, nr_instance;
};

SpMat read_data(std::string const path);

size_t const kNR_FIELD = 39;

struct Model
{
    Model(size_t const nr_feature, size_t const nr_factor) 
        : W(nr_feature*kNR_FIELD*nr_factor, 0), WG(nr_feature*kNR_FIELD*nr_factor, 1),
          nr_feature(nr_feature), nr_factor(nr_factor) {}
    std::vector<double> W, WG;
    const size_t nr_feature, nr_factor;
};

void save_model(Model const &model, std::string const &path);

Model load_model(std::string const &path);

FILE *open_c_file(std::string const &path, std::string const &mode);

std::vector<std::string> 
argv_to_args(int const argc, char const * const * const argv);

inline double qrsqrt(double x_)
{
    float x = static_cast<float>(x_);
    _mm_store_ss(&x, _mm_rsqrt_ps(_mm_load1_ps(&x)));
    return static_cast<double>(x);
}

inline double wTx(SpMat const &spmat, Model &model, size_t const i, 
    double const kappa=0, double const eta=0, double const lambda=0, 
    bool const do_update=false)
{
    size_t const nr_factor = model.nr_factor;

    double t = 0;
    for(size_t idx1 = spmat.P[i]; idx1 < spmat.P[i+1]; ++idx1)
    {
        size_t const j1 = spmat.X[idx1].j;
        size_t const f1 = spmat.X[idx1].f;
        double const v1 = spmat.X[idx1].v;

        for(size_t idx2 = idx1+1; idx2 < spmat.P[i+1]; ++idx2)
        {
            size_t const j2 = spmat.X[idx2].j;
            size_t const f2 = spmat.X[idx2].f;
            double const v2 = spmat.X[idx2].v;

            double * w1 = 
                model.W.data()+j1*kNR_FIELD*nr_factor+f2*nr_factor;
            double * wg1 = 
                model.WG.data()+j1*kNR_FIELD*nr_factor+f2*nr_factor;
            double * w2 = 
                model.W.data()+j2*kNR_FIELD*nr_factor+f1*nr_factor;
            double * wg2 = 
                model.WG.data()+j2*kNR_FIELD*nr_factor+f1*nr_factor;

            if(do_update)
            {
                for(size_t d = 0; d < nr_factor; ++d, ++w1, ++w2, ++wg1, ++wg2)
                {
                    double const g1 = lambda*(*w1) + kappa*v1*v2*(*w2);
                    double const g2 = lambda*(*w2) + kappa*v1*v2*(*w1);

                    *wg1 += g1*g1;
                    *wg2 += g2*g2;

                    *w1 -= eta*qrsqrt(*wg1)*g1;
                    *w2 -= eta*qrsqrt(*wg2)*g2;
                }
            }
            else
            {
                for(size_t d = 0; d < nr_factor; ++d, ++w1, ++w2)
                    t += v1*v2*(*w1)*(*w2);
            }
        }
    }

    return t;
}

double predict(SpMat const &spmat, Model &model, 
    std::string const &output_path = std::string(""));
#endif // _COMMON_H_
