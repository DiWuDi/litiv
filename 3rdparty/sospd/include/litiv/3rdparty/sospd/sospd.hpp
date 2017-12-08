#pragma once

#include "litiv/3rdparty/sospd/multilabel-energy.hpp"
#include "litiv/3rdparty/sospd/submodular-ibfs.hpp"

namespace sospd {

    /** Optimizer using Sum-of-submodular Primal Dual algorithm.
     *
     * Implements SoSPD algorithm from Fix, Wang, Zabih in CVPR 14.
     */
    template<typename ValueType, typename IndexType, typename LabelType, typename Flow = SubmodularIBFS<ValueType,IndexType>>
    class SoSPD {
        public:
            typedef MultilabelEnergy<ValueType,IndexType,LabelType> MLE;
            typedef typename MLE::REAL REAL;
            typedef typename MLE::VarId VarId;
            typedef typename MLE::Label Label;
            typedef sospd::Assgn Assgn;

            /** Proposal callbacks take as input the iteration number and current
             * labeling (as a vector of labels) and write the next proposal to the
             * final parameter.
             */
            typedef std::function<void(IndexType niter,const std::vector<Label>& current,std::vector<Label>& proposed)> ProposalCallback;

            /** Set up SoSPD to optimize a particular energy function
             *
             * \param energy Energy function to optimize.
             */
            explicit SoSPD(const MLE* energy);
            explicit SoSPD(const MLE* energy, SubmodularIBFSParams& params);

            /** Run SoSPD algorithm either to completion, or for a number of steps.
             *
             * Each iteration has a single proposal (determined by
             * SetProposalCallback), and solves a corresponding Sum-of-Submodular
             * flow problem.
             *
             * Resulting labeling can be queried from GetLabel.
             *
             * \param niters Number of iterations
             */
            void Solve(IndexType niters = std::numeric_limits<IndexType>::max());

            /** Return label of a node i, returns -1 if Solve has not been called.*/
            LabelType GetLabel(VarId i) const;

            /** Give hint that energy is expansion submodular. Enables optimizations
             * because we don't need to find submodular upper/lower bounds for the
             * function.
             */
            void SetExpansionSubmodular(bool b) { m_expansion_submodular = b; }

            /** Choose whether to use lower/upper bound in approximating function.
             */
            void SetLowerBound(bool b) { m_lower_bound = b; }

            /** Specify method for choosing proposals. */
            void SetProposalCallback(const ProposalCallback& pc) { m_pc = pc; }

            /** Set the proposal method to alpha-expansion
             *
             * Alpha-expansion proposals simply cycle through the labels, proposing
             * a constant labeling (i.e., all "alpha") at each iteration.
             */
            void SetAlphaExpansion() {
                m_pc = [&](IndexType,const std::vector<Label>&,std::vector<Label>&) {
                    AlphaProposal();
                };
            }

            /** Set the proposal method to best-height alpha-expansion
             *
             * Best-height alpha-expansion, instead of cycling through labels,
             * chooses the single alpha with the biggest sum of differences in
             * heights.
             */
            void SetHeightAlphaExpansion() {
                m_pc = [&](IndexType,const std::vector<Label>&,std::vector<Label>&) {
                    HeightAlphaProposal();
                };
            }

            /** Return lower bound on optimum, determined by current dual */
            double LowerBound();

            REAL dualVariable(IndexType alpha, VarId i, Label l) const;
            Flow* GetFlow() { return &m_ibfs; }

        private:
            typedef typename MLE::CliquePtr CliquePtr;
            typedef std::vector<REAL> LambdaAlpha;
            typedef std::vector<std::pair<IndexType,IndexType>> NodeNeighborList;
            typedef std::vector<NodeNeighborList> NodeCliqueList;

            REAL ComputeHeight(VarId, Label);
            REAL ComputeHeightDiff(VarId i, Label l1, Label l2) const;
            void SetupGraph(Flow& crf);
            // TODO(afix): redo this
            void SetupAlphaEnergy(Flow& crf);
            void InitialLabeling();
            void InitialDual();
            void InitialNodeCliqueList();
            bool InitialFusionLabeling();
            void PreEditDual(Flow& crf);
            bool UpdatePrimalDual(Flow& crf);
            void PostEditDual();
            void DualFit();
            REAL& Height(VarId i, Label l) { return m_heights[i*m_num_labels+l]; }

            REAL& dualVariable(IndexType alpha, VarId i, Label l);
            REAL dualVariable(const LambdaAlpha& lambdaAlpha,
                    VarId i, Label l) const;
            REAL& dualVariable(LambdaAlpha& lambdaAlpha,
                    VarId i, Label l);
            LambdaAlpha& lambdaAlpha(IndexType alpha);
            const LambdaAlpha& lambdaAlpha(IndexType alpha) const;

            // Move Proposals
            void HeightAlphaProposal();
            void AlphaProposal();

            const MLE* m_energy;
            // Unique ptr so we can forward declare?
            Flow m_ibfs;
            const IndexType m_num_labels;
            std::vector<Label> m_labels;
            /// The proposed labeling in a given iteration
            std::vector<Label> m_fusion_labels;
            // Factor this list back into a node list?
            NodeCliqueList m_node_clique_list;
            // FIXME(afix) change way m_dual is stored. Put lambda_alpha as separate
            // REAL* for each clique, indexed by i, l.
            std::vector<LambdaAlpha> m_dual;
            std::vector<REAL> m_heights;
            bool m_expansion_submodular;
            bool m_lower_bound;
            IndexType m_iter;
            ProposalCallback m_pc;
    };

} // namespace sospd

template<typename V, typename I, typename L, typename F>
inline sospd::SoSPD<V,I,L,F>::SoSPD(const MLE* energy)
        : m_energy(energy),
          m_num_labels(energy->numLabels()),
          m_labels(energy->numVars(), 0),
          m_fusion_labels(energy->numVars(), 0),
          m_expansion_submodular(false),
          m_lower_bound(false),
          m_iter(0),
          m_pc([&](I,const std::vector<Label>&,std::vector<Label>&) {HeightAlphaProposal();})
{ }

template<typename V, typename I, typename L, typename F>
inline sospd::SoSPD<V,I,L,F>::SoSPD(const MLE* energy, SubmodularIBFSParams& params)
        : m_energy(energy),
          m_ibfs(params),
          m_num_labels(energy->numLabels()),
          m_labels(energy->numVars(), 0),
          m_fusion_labels(energy->numVars(), 0),
          m_expansion_submodular(false),
          m_lower_bound(false),
          m_iter(0),
          m_pc([&](I,const std::vector<Label>&,std::vector<Label>&) {HeightAlphaProposal();})
{ }

template<typename V, typename I, typename L, typename F>
inline L sospd::SoSPD<V,I,L,F>::GetLabel(VarId i) const {
    return L(m_labels[i]);
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::InitialLabeling() {
    const VarId n = m_energy->numVars();
    for (VarId i = 0; i < n; ++i) {
        REAL best_cost = std::numeric_limits<REAL>::max();
        for (I l = 0; l < m_num_labels; ++l) {
            if (m_energy->unary(i, l) < best_cost) {
                best_cost = m_energy->unary(i, l);
                m_labels[i] = l;
            }
        }
    }
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::InitialDual() {
    // Initialize heights
    m_heights = std::vector<REAL>(m_energy->numVars()*m_num_labels, 0);
    for (VarId i = 0; i < m_energy->numVars(); ++i)
        for (Label l = 0; l < m_num_labels; ++l)
            Height(i, l) = m_energy->unary(i, l);

    m_dual.clear();
    Label labelBuf[32];
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const VarId* nodes = c.nodes();
        I k = I(c.size());
        ASSERT(k < I(32));
        for (I i = 0; i < k; ++i) {
            labelBuf[i] = m_labels[nodes[i]];
        }
        REAL energy = c.energy(labelBuf);
        m_dual.emplace_back(k*m_num_labels, 0);
        LambdaAlpha& lambda_a = m_dual.back();

        ASSERT(energy >= 0);
        REAL avg = energy / k;
        int remainder = int(energy) % k;
        for (I i = 0; i < k; ++i) {
            Label l = m_labels[nodes[i]];
            REAL& lambda_ail = dualVariable(lambda_a, i, l);
            lambda_ail = avg;
            if(int(i) < remainder) // Have to distribute remainder to maintain average
                lambda_ail += 1;
            Height(nodes[i], l) += lambda_ail;
        }
    }
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::InitialNodeCliqueList() {
    I n = I(m_labels.size());
    m_node_clique_list.clear();
    m_node_clique_list.resize(n);

    I clique_index = 0;
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const VarId* nodes = c.nodes();
        const I k = I(c.size());
        for (I i = 0; i < k; ++i) {
            m_node_clique_list[nodes[i]].push_back(std::make_pair(clique_index, i));
        }
        ++clique_index;
    }
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::PreEditDual(F& crf) {
    auto& fixedVars = crf.Params().fixedVars;
    fixedVars.resize(m_labels.size());
    for (I i = 0; i < I(m_labels.size()); ++i)
        fixedVars[i] = (m_labels[i] == m_fusion_labels[i]);

    // Allocate all the buffers we need in one place, resize as necessary
    Label label_buf[32];
    std::vector<Label> current_labels;
    std::vector<Label> fusion_labels;
    std::vector<REAL> psi;
    std::vector<REAL> current_lambda;
    std::vector<REAL> fusion_lambda;

    auto& ibfs_cliques = crf.Graph().GetCliques();
    ASSERT(ibfs_cliques.size() == m_energy->cliques().size());
    I clique_index = 0;
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const I k = I(c.size());
        ASSERT(k < I(32));

        auto& lambda_a = lambdaAlpha(clique_index);

        auto& ibfs_c = ibfs_cliques[clique_index];
        ASSERT(k == ibfs_c.Size());
        std::vector<REAL>& energy_table = ibfs_c.EnergyTable();
        sospd::Assgn max_assgn = 1u << k;
        ASSERT(energy_table.size() == max_assgn);

        psi.resize(k);
        current_labels.resize(k);
        fusion_labels.resize(k);
        current_lambda.resize(k);
        fusion_lambda.resize(k);
        for (I i = 0; i < k; ++i) {
            current_labels[i] = m_labels[c.nodes()[i]];
            fusion_labels[i] = m_fusion_labels[c.nodes()[i]];
            /*
             *ASSERT(0 <= c.nodes()[i] && c.nodes()[i] < m_labels.size());
             *ASSERT(0 <= current_labels[i] && current_labels[i] < m_num_labels);
             *ASSERT(0 <= fusion_labels[i] && fusion_labels[i] < m_num_labels);
             */
            current_lambda[i] = dualVariable(lambda_a, i, current_labels[i]);
            fusion_lambda[i] = dualVariable(lambda_a, i, fusion_labels[i]);
        }

        // Compute costs of all fusion assignments
        {
            Assgn last_gray = 0;
            for (I i_idx = 0; i_idx < k; ++i_idx)
                label_buf[i_idx] = current_labels[i_idx];
            energy_table[0] = c.energy(label_buf);
            for (Assgn a = 1; a < max_assgn; ++a) {
                Assgn gray = a ^ (a >> 1);
                Assgn diff = gray ^ last_gray;
                int changed_idx = __builtin_ctz(diff);
                if (diff & gray)
                    label_buf[changed_idx] = fusion_labels[changed_idx];
                else
                    label_buf[changed_idx] = current_labels[changed_idx];
                last_gray = gray;
                energy_table[gray] = c.energy(label_buf);
            }
        }

        // Compute the residual function
        // g(S) - lambda_fusion(S) - lambda_current(C\S)
        sospd::SubtractLinear(k, energy_table, fusion_lambda, current_lambda);
        ASSERT(energy_table[0] == 0); // Check tightness of current labeling

        ++clique_index;
    }
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL sospd::SoSPD<V,I,L,F>::ComputeHeight(VarId i, Label x) {
    REAL ret = m_energy->unary(i, x);
    for (const auto& p : m_node_clique_list[i]) {
        ret += dualVariable(p.first, p.second, x);
    }
    return ret;
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL sospd::SoSPD<V,I,L,F>::ComputeHeightDiff(VarId i, Label l1, Label l2) const {
    REAL ret = m_energy->unary(i, l1) - m_energy->unary(i, l2);
    for (const auto& p : m_node_clique_list[i]) {
        ret += dualVariable(p.first, p.second, l1)
               - dualVariable(p.first, p.second, l2);
    }
    return ret;
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::SetupGraph(F& crf) {
    const I n = I(m_labels.size());
    crf.AddNode(n);
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const I k = I(c.size());
        ASSERT(k < I(32));
        const I max_assgn = 1u << k;
        std::vector<typename F::NodeId> nodes(c.nodes(), c.nodes() + c.size());
        crf.AddClique(nodes, std::vector<REAL>(max_assgn, 0));
    }
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::SetupAlphaEnergy(F& crf) {
    const I n = I(m_labels.size());
    crf.ClearUnaries();
    crf.AddConstantTerm(-crf.GetConstantTerm());
    for (I i = 0; i < n; ++i) {
        REAL height_diff = ComputeHeightDiff(i, m_labels[i], m_fusion_labels[i]);
        if (height_diff > 0) {
            crf.AddUnaryTerm(i, height_diff, 0);
        }
        else {
            crf.AddUnaryTerm(i, 0, -height_diff);
        }
    }
}

template<typename V, typename I, typename L, typename F>
inline bool sospd::SoSPD<V,I,L,F>::UpdatePrimalDual(F& crf) {
    bool ret = false;
    SetupAlphaEnergy(crf);
    crf.Solve();
    VarId n = m_labels.size();
    for (VarId i = 0; i < n; ++i) {
        int crf_label = crf.GetLabel(i);
        if (crf_label == 1) {
            Label alpha = m_fusion_labels[i];
            if (m_labels[i] != alpha) ret = true;
            m_labels[i] = alpha;
        }
    }
    const auto& clique = crf.Graph().GetCliques();
    I i = 0;
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        auto& ibfs_c = clique[i];
        const std::vector<REAL>& phiCi = ibfs_c.AlphaCi();
        for (I j = 0; j < I(phiCi.size()); ++j) {
            dualVariable(i, j, m_fusion_labels[c.nodes()[j]]) += phiCi[j];
            Height(c.nodes()[j], m_fusion_labels[c.nodes()[j]]) += phiCi[j];
        }
        ++i;
    }
    return ret;
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::PostEditDual() {
    Label labelBuf[32];
    I clique_index = 0;
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const VarId* nodes = c.nodes();
        I k = I(c.size());
        ASSERT(k < I(32));
        REAL lambdaSum = 0;
        for (I i = 0; i < k; ++i) {
            labelBuf[i] = m_labels[nodes[i]];
            lambdaSum += dualVariable(clique_index, i, labelBuf[i]);
        }
        REAL energy = c.energy(labelBuf);
        REAL correction = energy - lambdaSum;
        if (correction > 0) {
            std::cout << "Bad clique in PostEditDual!\t Id:" << clique_index << "\n";
            std::cout << "Correction: " << correction << "\tenergy: " << energy << "\tlambdaSum " << lambdaSum << "\n";
            const auto& c2 = m_ibfs.Graph().GetCliques()[clique_index];
            std::cout << "EnergyTable: ";
            for (const auto& e : c2.EnergyTable())
                std::cout << e << ", ";
            std::cout << "\n";
        }
        ASSERT(correction <= 0);
        REAL avg = correction / k;
        int remainder = int(correction) % k;
        if (remainder < 0) {
            avg -= 1;
            remainder += k;
        }
        for (I i = 0; i < k; ++i) {
            auto& lambda_ail = dualVariable(clique_index,  i, labelBuf[i]);
            Height(nodes[i], labelBuf[i]) -= lambda_ail;
            lambda_ail += avg;
            if (int(i) < remainder)
                lambda_ail += 1;
            Height(nodes[i], labelBuf[i]) += lambda_ail;
        }
        ++clique_index;
    }
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::DualFit() {
    // FIXME: This is the only function that doesn't work with integer division.
    // It's also not really used for anything at the moment
    /*
	for (I i = 0; i < I(m_dual.size()); ++i)
		for (I j = 0; j < I(m_dual[i].size()); ++j)
			for (I k = 0; k < I(m_dual[i][j].size()); ++k)
				m_dual[i][j][k] /= (m_mu * m_rho);
                */
    ASSERT(false /* unimplemented */);
}

template<typename V, typename I, typename L, typename F>
inline bool sospd::SoSPD<V,I,L,F>::InitialFusionLabeling() {
    m_pc(m_iter, m_labels, m_fusion_labels);
    bool allDiff = false;
    for (I i = 0; i < I(m_labels.size()); ++i) {
        if (m_fusion_labels[i] < 0) m_fusion_labels[i] = 0;
        if (m_fusion_labels[i] >= m_num_labels) m_fusion_labels[i] = m_num_labels-1;
        if (m_labels[i] != m_fusion_labels[i])
            allDiff = true;
    }
    return allDiff;
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::HeightAlphaProposal() {
    const I n = I(m_labels.size());
    REAL max_s_capacity = 0;
    Label alpha = 0;
    for (Label l = 0; l < m_num_labels; ++l) {
        REAL s_capacity = 0;
        for (I i = 0; i < n; ++i) {
            REAL diff = Height(i, m_labels[i]) - Height(i, l);
            if (diff > V(0))
                s_capacity += diff;
        }
        if (s_capacity > max_s_capacity) {
            max_s_capacity = s_capacity;
            alpha = l;
        }
    }
    for (I i = 0; i < n; ++i)
        m_fusion_labels[i] = alpha;
}

template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::AlphaProposal() {
    Label alpha = m_iter % m_num_labels;
    const I n = I(m_labels.size());
    for (I i = 0; i < n; ++i)
        m_fusion_labels[i] = alpha;
}


template<typename V, typename I, typename L, typename F>
inline void sospd::SoSPD<V,I,L,F>::Solve(I niters) {
    if (m_iter == I(0)) {
        SetupGraph(m_ibfs);
        InitialLabeling();
        InitialDual();
        InitialNodeCliqueList();
    }
#ifdef PROGRESS_DISPLAY
    REAL energy = m_energy->ComputeEnergy(m_labels);
		std::cout << "Iteration " << m_iter << ": " << energy << std::endl;
#endif
    bool labelChanged = true;
    I this_iter = 0;
    while (labelChanged && this_iter < niters){
        labelChanged = InitialFusionLabeling();
        if (!labelChanged) break;
        PreEditDual(m_ibfs);
        UpdatePrimalDual(m_ibfs);
        PostEditDual();
        this_iter++;
        m_iter++;
#ifdef PROGRESS_DISPLAY
        energy = m_energy->ComputeEnergy(m_labels);
			std::cout << "Iteration " << m_iter << ": " << energy << std::endl;
#endif
    }
    //LowerBound();
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL sospd::SoSPD<V,I,L,F>::dualVariable(I alpha, VarId i, Label l) const {
    return m_dual[alpha][i*m_num_labels+l];
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL& sospd::SoSPD<V,I,L,F>::dualVariable(I alpha, VarId i, Label l) {
    return m_dual[alpha][i*m_num_labels+l];
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL sospd::SoSPD<V,I,L,F>::dualVariable(const LambdaAlpha& lambdaAlpha, VarId i, Label l) const {
    return lambdaAlpha[i*m_num_labels+l];
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::REAL& sospd::SoSPD<V,I,L,F>::dualVariable(LambdaAlpha& lambdaAlpha, VarId i, Label l) {
    return lambdaAlpha[i*m_num_labels+l];
}

template<typename V, typename I, typename L, typename F>
inline typename sospd::SoSPD<V,I,L,F>::LambdaAlpha& sospd::SoSPD<V,I,L,F>::lambdaAlpha(I alpha) {
    return m_dual[alpha];
}

template<typename V, typename I, typename L, typename F>
inline const typename sospd::SoSPD<V,I,L,F>::LambdaAlpha& sospd::SoSPD<V,I,L,F>::lambdaAlpha(I alpha) const {
    return m_dual[alpha];
}

template<typename V, typename I, typename L, typename F>
inline double sospd::SoSPD<V,I,L,F>::LowerBound() {
    std::cout << "Computing Lower Bound\n";
    double max_ratio = 0;
    I clique_index = 0;
    for (const CliquePtr& cp : m_energy->cliques()) {
        const Clique<V,I,L>& c = *cp;
        const I k = I(c.size());
        ASSERT(k == 3); // Lower bound doesn't work for larger numbers
        Label buf[3];
        for (buf[0] = 0; buf[0] < m_num_labels; ++buf[0]) {
            for (buf[1] = 0; buf[1] < m_num_labels; ++buf[1]) {
                for (buf[2] = 0; buf[2] < m_num_labels; ++buf[2]) {
                    REAL energy = c.energy(buf);
                    REAL dualSum = dualVariable(clique_index, 0, buf[0])
                                   + dualVariable(clique_index, 1, buf[1])
                                   + dualVariable(clique_index, 2, buf[2]);
                    if (energy == 0) {
                        for (I i = 0; i < I(3); ++i) {
                            if (buf[i] != m_labels[c.nodes()[i]]) {
                                dualVariable(clique_index, i, buf[i]) -= dualSum - energy;
                                Height(c.nodes()[i], buf[i]) -= dualSum - energy;
                                dualSum = energy;
                                break;
                            }
                        }
                        ASSERT(dualSum == energy);
                    } else {
                        max_ratio = std::max(max_ratio, double(dualSum)/double(energy));
                    }
                }
            }
        }
        clique_index++;
    }
    REAL dual_objective = 0;
    for (VarId i = 0; i < m_energy->numVars(); ++i) {
        REAL min_height = std::numeric_limits<REAL>::max();
        for (Label l = 0; l < m_num_labels; ++l)
            min_height = std::min(min_height, Height(i, l));
        dual_objective += min_height;
    }
    std::cout << "Max Ratio: " << max_ratio << "\n";
    std::cout << "Dual objective: " << dual_objective << "\n";
    return dual_objective / max_ratio;
}