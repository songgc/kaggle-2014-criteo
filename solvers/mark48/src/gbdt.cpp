#include <limits>
#include <numeric>
#include <algorithm>
#include <thread>
#include <omp.h>

#include "gbdt.h"
#include "timer.h"

namespace {

float calc_bias(std::vector<float> const &Y)
{
    float y_bar = static_cast<float>(std::accumulate(Y.begin(), Y.end(), 0.0) /
        static_cast<double>(Y.size()));
    return static_cast<float>(log((1.0f+y_bar)/(1.0f-y_bar)));
}

template<typename Type>
void clean_vector(std::vector<Type> &vec)
{
    vec.clear();
    vec.shrink_to_fit();
}

void update_F(Problem const &problem, CART const &tree, std::vector<float> &F)
{
    uint32_t const nr_field = problem.nr_field; 
    uint32_t const nr_sparse_field = problem.nr_sparse_field;
    std::vector<uint32_t> const &SJ = problem.SJ;
    std::vector<uint64_t> const &SJP = problem.SJP;

    #pragma omp parallel for schedule(static)
    for(uint32_t i = 0; i < problem.nr_instance; ++i)
    {
        std::vector<float> x(nr_field+nr_sparse_field, 0);
        for(uint32_t j = 0; j < nr_field; ++j)
            x[j] = problem.X[j][i].v;
        for(uint64_t p = SJP[i]; p < SJP[i+1]; ++p)
            x[SJ[p]+nr_field] = 1;
        F[i] += tree.predict(x.data()).second;
    }
}

} //unnamed namespace

uint32_t CART::max_depth = 7;
uint32_t CART::max_tnodes = static_cast<uint32_t>(pow(2, CART::max_depth+1));
std::mutex CART::mtx;
bool CART::verbose = false;

void CART::fit(Problem const &problem, std::vector<float> const &R, 
    std::vector<float> &F1)
{
    struct Location
    {
        Location() : tnode_idx(1), r(0), shrinked(false) {}
        uint32_t tnode_idx;
        float r;
        bool shrinked;
    };

    uint32_t const nr_field = problem.nr_field;
    uint32_t const nr_sparse_field = problem.nr_sparse_field;
    uint32_t const nr_instance = problem.nr_instance;

    std::vector<Location> locations(nr_instance);
    #pragma omp parallel for schedule(static)
    for(uint32_t i = 0; i < nr_instance; ++i)
        locations[i].r = R[i];
    for(uint32_t d = 0, idx_offset = 1; d < max_depth; ++d, idx_offset *= 2)
    {
        struct Meta
        {
            Meta() : sl(0), s(0), nl(0), n(0), v(0.0f/0.0f) {}
            double sl, s;
            uint32_t nl, n;
            float v;
        };

        uint32_t const max_nr_leaf = static_cast<uint32_t>(pow(2, d));
        std::vector<Meta> metas0(max_nr_leaf);

        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location &location = locations[i];
            if(location.shrinked)
                continue;

            Meta &meta = metas0[location.tnode_idx-idx_offset];
            meta.s += location.r;
            ++meta.n;
        }

        std::vector<double> best_eses(max_nr_leaf, 0);
        for(uint32_t idx = 0; idx < max_nr_leaf; ++idx)
        {
            Meta const &meta = metas0[idx];
            best_eses[idx] = meta.s*meta.s/static_cast<double>(meta.n);
        }

        #pragma omp parallel for schedule(dynamic)
        for(uint32_t j = 0; j < nr_field; ++j)
        {
            std::vector<Meta> metas = metas0;

            for(uint32_t i = 0; i < nr_instance; ++i)
            {
                Node const &dnode = problem.X[j][i];
                Location const &location = locations[dnode.i];
                if(location.shrinked)
                    continue;

                TreeNode &tnode = tnodes[location.tnode_idx];
                Meta &meta = metas[location.tnode_idx-idx_offset];

                if(dnode.v != meta.v)
                {
                    double const sr = meta.s - meta.sl;
                    uint32_t const nr = meta.n - meta.nl;
                    double const current_ese = 
                        (meta.sl*meta.sl)/static_cast<double>(meta.nl) + 
                        (sr*sr)/static_cast<double>(nr);

                    #pragma omp critical
                    {
                        double &best_ese = 
                            best_eses[location.tnode_idx-idx_offset];
                        if((current_ese > best_ese) || 
                           (current_ese == best_ese && 
                            static_cast<int>(j) < tnode.feature))
                        {
                            best_ese = current_ese;
                            tnode.feature = j;
                            tnode.threshold = dnode.v;
                        }
                    }
                }

                meta.sl += location.r;
                ++meta.nl;
                meta.v = dnode.v;
            }
        }

        #pragma omp parallel for schedule(dynamic)
        for(uint32_t j = 0; j < nr_sparse_field; ++j)
        {
            std::vector<Meta> metas = metas0;
            for(uint64_t p = problem.SIP[j]; p < problem.SIP[j+1]; ++p)
            {
                Location const &location = locations[problem.SI[p]];
                if(location.shrinked)
                    continue;
                Meta &meta = metas[location.tnode_idx-idx_offset];
                meta.sl += location.r;
                ++meta.nl;
            }

            for(uint32_t leaf_idx = 0; leaf_idx < max_nr_leaf; ++leaf_idx)
            {
                Meta const &meta = metas[leaf_idx];
                if(meta.nl == 0)
                    continue;
                
                TreeNode &tnode = tnodes[idx_offset+leaf_idx];

                double const sr = meta.s - meta.sl;
                uint32_t const nr = meta.n - meta.nl;
                double const current_ese = 
                    (meta.sl*meta.sl)/static_cast<double>(meta.nl) + 
                    (sr*sr)/static_cast<double>(nr);

                #pragma omp critical
                {
                    double &best_ese = best_eses[leaf_idx];
                    if((current_ese > best_ese) || 
                       (current_ese == best_ese && 
                        static_cast<int>(j+nr_field) < tnode.feature))
                    {
                        best_ese = current_ese;
                        tnode.feature = j+nr_field;
                        tnode.threshold = 1;
                    }
                }
            }
        }

        #pragma omp parallel for schedule(static)
        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location &location = locations[i];
            if(location.shrinked)
                continue;

            uint32_t &tnode_idx = location.tnode_idx;
            TreeNode &tnode = tnodes[tnode_idx];
            if(tnode.feature == -1)
            {
                location.shrinked = true;
            }
            else if(static_cast<uint32_t>(tnode.feature) < nr_field)
            {
                if(problem.Z[tnode.feature][i].v < tnode.threshold)
                    tnode_idx = 2*tnode_idx; 
                else
                    tnode_idx = 2*tnode_idx+1; 
            }
            else
            {
                bool is_one = false;
                for(uint64_t p = problem.SJP[i]; p < problem.SJP[i+1]; ++p) 
                {
                    if(problem.SJ[p] == static_cast<uint32_t>(tnode.feature-nr_field))
                    {
                        is_one = true;
                        break;
                    }
                }
                if(!is_one)
                    tnode_idx = 2*tnode_idx; 
                else
                    tnode_idx = 2*tnode_idx+1; 
            }
        }

        uint32_t max_nr_leaf_next = max_nr_leaf*2;
        uint32_t idx_offset_next = idx_offset*2;
        std::vector<uint32_t> counter(max_nr_leaf_next, 0);
        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location const &location = locations[i];
            if(location.shrinked)
                continue;
            ++counter[locations[i].tnode_idx-idx_offset_next];
        }

        #pragma omp parallel for schedule(static)
        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location &location = locations[i]; 
            if(location.shrinked)
                continue;
            if(counter[location.tnode_idx-idx_offset_next] < 100)
                location.shrinked = true;
        }
    }

    std::vector<std::pair<double, double>> 
        tmp(max_tnodes, std::make_pair(0, 0));
    for(uint32_t i = 0; i < nr_instance; ++i)
    {
        Location const &location = locations[i];
        tmp[location.tnode_idx].first += location.r;
        tmp[location.tnode_idx].second += fabs(location.r)*(1-fabs(location.r));
    }

    for(uint32_t tnode_idx = 1; tnode_idx <= max_tnodes; ++tnode_idx)
    {
        double a, b;
        std::tie(a, b) = tmp[tnode_idx];
        tnodes[tnode_idx].gamma = (b <= 1e-12)? 0 : static_cast<float>(a/b);
    }

    #pragma omp parallel for schedule(static)
    for(uint32_t i = 0; i < nr_instance; ++i)
        F1[i] = tnodes[locations[i].tnode_idx].gamma;
}

std::pair<uint32_t, float> CART::predict(float const * const x) const
{
    uint32_t tnode_idx = 1;
    for(uint32_t d = 0; d <= max_depth; ++d)
    {
        TreeNode const &tnode = tnodes[tnode_idx];
        if(tnode.feature == -1)
            return std::make_pair(tnode.idx, tnode.gamma);

        if(x[tnode.feature] < tnode.threshold)
            tnode_idx = tnode_idx*2;
        else
            tnode_idx = tnode_idx*2+1;
    }

    return std::make_pair(-1, -1);
}

void GBDT::fit(Problem const &Tr, Problem const &Va)
{
    bias = calc_bias(Tr.Y);

    std::vector<float> F_Tr(Tr.nr_instance, bias), F_Va(Va.nr_instance, bias);

    Timer timer;
    for(uint32_t t = 0; t < trees.size(); ++t)
    {
        timer.tic();

        std::vector<float> const &Y = Tr.Y;
        std::vector<float> R(Tr.nr_instance), F1(Tr.nr_instance);

        #pragma omp parallel for schedule(static)
        for(uint32_t i = 0; i < Tr.nr_instance; ++i) 
            R[i] = static_cast<float>(Y[i]/(1+exp(Y[i]*F_Tr[i])));

        trees[t].fit(Tr, R, F1);

        double Tr_loss = 0;
        #pragma omp parallel for schedule(static) reduction(+: Tr_loss)
        for(uint32_t i = 0; i < Tr.nr_instance; ++i) 
        {
            F_Tr[i] += F1[i];
            Tr_loss += log(1+exp(-Y[i]*F_Tr[i]));
        }

        printf("%3d %8.2f %10.5f", t, timer.toc(), 
            Tr_loss/static_cast<double>(Tr.Y.size()));

        if(Va.nr_instance != 0)
        {
            update_F(Va, trees[t], F_Va);

            double Va_loss = 0;
            #pragma omp parallel for schedule(static) reduction(+: Va_loss)
            for(uint32_t i = 0; i < Va.nr_instance; ++i) 
                Va_loss += log(1+exp(-Va.Y[i]*F_Va[i]));

            printf(" %10.5f", Va_loss/static_cast<double>(Va.nr_instance));
        }

        printf("\n");
        fflush(stdout);
    }
}

float GBDT::predict(float const * const x) const
{
    float s = bias;
    for(auto &tree : trees)
        s += tree.predict(x).second;
    return s;
}

std::vector<uint32_t> GBDT::get_indices(float const * const x) const
{
    uint32_t const nr_tree = static_cast<uint32_t>(trees.size());

    std::vector<uint32_t> indices(nr_tree);
    for(uint32_t t = 0; t < nr_tree; ++t)
        indices[t] = trees[t].predict(x).first;
    return indices;
}
