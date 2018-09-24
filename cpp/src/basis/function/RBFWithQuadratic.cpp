////////////////////////////////////////////////////////////////////////////////
#include <polyfem/RBFWithQuadratic.hpp>
#include <polyfem/Types.hpp>
#include <polyfem/MatrixUtils.hpp>
#include <polyfem/Logger.hpp>

#include <polyfem/Laplacian.hpp>

#include <igl/Timer.h>

#include <Eigen/Dense>

#include <iostream>
#include <fstream>
#include <array>
////////////////////////////////////////////////////////////////////////////////

// #define VERBOSE

using namespace polyfem;

namespace {

// Harmonic kernel
double kernel(const bool is_volume, const double r) {
	if (r < 1e-8) { return 0; }

	if (is_volume) {
		return 1/r;
	} else {
		return log(r);
	}
}

double kernel_prime(const bool is_volume, const double r) {
	if (r < 1e-8) { return 0; }

	if(is_volume) {
		return -1/(r*r);
	} else {
		return 1/r;
	}
}

// Biharmonic kernel (2d only)
// double kernel(const bool is_volume, const double r) {
// 	assert(!is_volume);
// 	if (r < 1e-8) { return 0; }

// 	return r * r * (log(r)-1);
// }

// double kernel_prime(const bool is_volume, const double r) {
// 	assert(!is_volume);
// 	if (r < 1e-8) { return 0; }

// 	return r * ( 2 * log(r) - 1);
// }

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////

RBFWithQuadratic::RBFWithQuadratic(
		const Eigen::MatrixXd &centers,
		const Eigen::MatrixXd &collocation_points,
		const Eigen::MatrixXd &local_basis_integral,
		const Quadrature &quadr,
		Eigen::MatrixXd &rhs,
		bool with_constraints)
	: centers_(centers)
{
	// centers_.resize(0, centers.cols());
	compute_weights(collocation_points, local_basis_integral, quadr, rhs, with_constraints);
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::basis(const int local_index, const Eigen::MatrixXd &samples, Eigen::MatrixXd &val) const {
	Eigen::MatrixXd tmp;
	bases_values(samples, tmp);
	val = tmp.col(local_index);
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::grad(const int local_index, const Eigen::MatrixXd &samples, Eigen::MatrixXd &val) const {
	Eigen::MatrixXd tmp;
	const int dim = centers_.cols();
	val.resize(samples.rows(), dim);
	for (int d = 0; d < dim; ++d) {
		bases_grads(d, samples, tmp);
		val.col(d) = tmp.col(local_index);
	}
}

////////////////////////////////////////////////////////////////////////////////

void RBFWithQuadratic::bases_values(const Eigen::MatrixXd &samples, Eigen::MatrixXd &val) const {
	// Compute A
	Eigen::MatrixXd A;
	compute_kernels_matrix(samples, A);

	// Multiply by the weights
	val = A * weights_;
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::bases_grads(const int axis, const Eigen::MatrixXd &samples, Eigen::MatrixXd &val) const {
	const int num_kernels = centers_.rows();
	const int dim = (is_volume() ? 3 : 2);

	// Compute ∇xA
	Eigen::MatrixXd A_prime(samples.rows(), num_kernels + 1 + dim + dim*(dim+1)/2);
	A_prime.setZero();

	for (int j = 0; j < num_kernels; ++j) {
		A_prime.col(j) = (samples.rowwise() - centers_.row(j)).rowwise().norm().unaryExpr([this](double x)
			{ return kernel_prime(is_volume(), x) / x; });
		A_prime.col(j) = (samples.col(axis).array() - centers_(j, axis)) * A_prime.col(j).array();
	}
	// Linear terms
	A_prime.middleCols(num_kernels + 1 + axis, 1).setOnes();
	// Mixed terms
	if (dim == 2) {
		A_prime.col(num_kernels + 1 + dim) = samples.col(1 - axis);
	} else {
		A_prime.col(num_kernels + 1 + dim + axis) = samples.col((axis+1)%dim);
		A_prime.col(num_kernels + 1 + dim + (axis+2)%dim) = samples.col((axis+2)%dim);
	}
	// Quadratic terms
	A_prime.rightCols(dim).col(axis) = 2.0 * samples.col(axis);

	// Apply weights
	val = A_prime * weights_;
}

////////////////////////////////////////////////////////////////////////////////
//
// For each FEM basis φ that is nonzero on the element E, we want to
// solve the least square system A w = rhs, where:
//     ┏                                     ┓
//     ┃ ψ_k(pi) ... 1 xi yi xi*yi xi^2 yi^2 ┃
// A = ┃   ┊        ┊  ┊  ┊   ┊    ┊    ┊    ┃ ∊ ℝ^{#S x (#K+1+dim+dim*(dim+1)/2)}
//     ┃   ┊        ┊  ┊  ┊   ┊    ┊    ┊    ┃
//     ┗                                     ┛
//     ┏                                 ┓^⊤
// w = ┃ w_k ... a00 a10 a01 a11 a20 a02 ┃   ∊ ℝ^{#K+1+dim+dim*(dim+1)/2}
//     ┗                                 ┛
// - A is the RBF kernels evaluated over the collocation points (#S)
// - b is the expected value of the basis sampled on the boundary (#S)
// - w is the weight of the kernels defining the basis
// - pi = (xi, yi) is the i-th collocation point
//
// Moreover, we want to impose a constraint on the weight vector w so that each
// monomial Q(x,y) = x^α*y^β with α+β <= 2 is in the span of the FEM bases {φ_j}_j.
//
// In the case of Laplace's equation, we recall the weak form of the PDE as:
//
//   Find u such that: ∫_Ω Δu v = - ∫_Ω ∇u·∇v   ∀ v
//
// For our bases to exactly represent a monomial Q(x,y), it means that its
// approximation by the finite element bases {φ_j}_j must be equal to Q(x,y).
// In particular, for any φ_j that is nonzero on the polyhedral element E, we must have:
//
//   ∫_{𝘅 in Ω} ΔQ(𝘅) φ_j(𝘅) d𝘅  = - ∫_{𝘅 \in Ω} ∇Q(𝘅)·∇φ_j(𝘅) d𝘅     (1)
//
// Now, for each of the 5 non-constant monomials (9 in 3D), we need to compute
// Δ(x^α*y^β). For (α,β) ∊ {(1,0), (0,1), (1,1), (2,0), (0,2)}, this yields
// the following equalities:
//
//     Δx  = 0      (2a)
//     Δy  = 0      (2b)
//     Δxy = 0      (2c)
//     Δx² = 1      (2d)
//     Δy² = 1      (2e)
//
// If we plug these back into (1), and split the integral between the polyhedral
// element E and Ω\E, we obtain the following constraints:
//
// ∫_E ∇Q·∇φ_j + ∫_E ΔQ φ_j = - ∫_{Ω\E} ∇Q·∇φ_j - ∫_{Ω\E} ΔQ φ_j    (3)
//
// Note that the right-hand side of (3) is already known, since no two polyhedral
// cells are adjacent to each other, and the bases overlapping a polyhedron vanish
// on the boundary of the domain ∂Ω. This right-hand side is computed in advance
// and passed to our functions as in argument `local_basis_integral`.
//
// The left-hand side of equation (3) reduces to the following (in 2D):
//
//     ∫_E ∇x(φ_j) = c10                       (4a)
//     ∫_E ∇y(φ_j) = c01                       (4b)
//     ∫_E (y·∇x(φ_j) + y·∇x(φ_jj)) = c11      (4c)
//     ∫_E 2x·∇x(φ_j) + ∫_E 2 φ_j = c20        (4d)
//     ∫_E 2y·∇y(φ_j) + ∫_E 2 φ_j = c02        (4e)
//
// The next step is to express the basis φ_j in terms of the harmonic kernels and
// quadratic polynomials:
//
//     φ_j(x,y) = Σ_k w_k ψ_k(x,y) + a00 + a10*x + a01*y + a11*x*y + a20*x² + a02*y²
//
// The five equations in (4) become:
//
//    Σ_j w_k ∫∇x(ψ_k) + a10 |E| + a11 ∫y + a20 ∫2x = c10
//    Σ_j w_k ∫∇y(ψ_k) + a01 |E| + a11 ∫x + a02 ∫2y = c01
//    Σ_j w_k (∫y·∇x(ψ_k) + ∫x·∇y(ψ_k)) + a10 ∫y + a01 ∫x + a11 (∫x²+∫y²) + a20 2∫xy + a02 2∫xy = c11
//    Σ_j w_k (2∫x·∇x(ψ_k) + 2ψ_k) + a10 4∫x + a01 2∫y + a11 4∫xy + a20 6∫x² + a02 2∫y² = c20
//    Σ_j w_k (2∫y·∇y(ψ_k) + 2ψ_k) + a10 2∫x + a01 4∫y + a11 4∫xy + a20 2∫x² + a02 6∫y² = c02
//
// This system gives us a relationship between the fives a10, a01, a11, a20, a02
// and the rest of the w_k + a constant translation term. We can write down the
// corresponding system:
//
//       a10   a01   a11   a20   a02
//     ┏                              ┓             ┏     ┓
//     ┃ |E|         ∫y    2∫x        ┃             ┃ w_k ┃
//     ┃                              ┃             ┃  ┊  ┃
//     ┃       |E|   ∫x          2∫y  ┃             ┃  ┊  ┃
//     ┃                              ┃             ┃  ┊  ┃
// M = ┃  ∫y   ∫x  ∫x²+∫y² 2∫xy  2∫xy ┃ = \tilde{L} ┃  ┊  ┃ + \tilde{t}
//     ┃                              ┃             ┃  ┊  ┃
//     ┃ 4∫x  2∫y  4∫xy    6∫x²  2∫y² ┃             ┃  ┊  ┃
//     ┃                              ┃             ┃w_#K ┃
//     ┃ 2∫x  4∫y  4∫xy    2∫x²  6∫y² ┃             ┃ a00 ┃
//     ┗                              ┛             ┗     ┛
//
// Now, if we want to express w as w = Lv + t, and solve our least-square
// system as before, we need to invert M and compute L and t in terms of
// \tilde{L} and \tilde{t}
//
//     ┏                  ┓
//     ┃   1              ┃
//     ┃       1          ┃
//     ┃          ·       ┃
// L = ┃             ·    ┃ ∊ ℝ^{ (#K+1+dim+dim*(dim+1)/2) x (#K+1}) }
//     ┃                1 ┃
//     ┃ M^{-1} \tilde{L} ┃
//     ┗                  ┛
//     ┏                  ┓
//     ┃        0         ┃
//     ┃        ┊         ┃
// t = ┃        ┊         ┃ ∊ ℝ^{#K+1+dim+dim*(dim+1)/2}
//     ┃        0         ┃
//     ┃ M^{-1} \tilde{t} ┃
//     ┗                  ┛
// After solving the new least square system A L v = rhs - A t, we can retrieve
// w = L v
//
////////////////////////////////////////////////////////////////////////////////


void RBFWithQuadratic::compute_kernels_matrix(const Eigen::MatrixXd &samples, Eigen::MatrixXd &A) const {
	// Compute A
	const int num_kernels = centers_.rows();
	const int dim = (is_volume() ? 3 : 2);

	A.resize(samples.rows(), num_kernels + 1 + dim + dim*(dim+1)/2);
	for (int j = 0; j < num_kernels; ++j) {
		A.col(j) = (samples.rowwise() - centers_.row(j)).rowwise().norm().unaryExpr([this](double x)
			{ return kernel(is_volume(), x); });
	}
	A.col(num_kernels).setOnes(); // constant term
	A.middleCols(num_kernels + 1, dim) = samples; // linear terms
	if (dim == 2) {
		A.middleCols(num_kernels + dim + 1, 1) = samples.rowwise().prod(); // mixed terms
	} else if (dim == 3) {
		A.middleCols(num_kernels + dim + 1, 3) = samples;
		A.middleCols(num_kernels + dim + 1 + 0, 1).array() *= samples.col(1).array();
		A.middleCols(num_kernels + dim + 1 + 1, 1).array() *= samples.col(2).array();
		A.middleCols(num_kernels + dim + 1 + 2, 1).array() *= samples.col(0).array();
	}
	A.rightCols(dim) = samples.array().square(); // quadratic terms
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::compute_constraints_matrix_2d(
	const int num_bases,
	const Quadrature &quadr,
	const Eigen::MatrixXd &local_basis_integral,
	Eigen::MatrixXd &L,
	Eigen::MatrixXd &t) const
{
	const int num_kernels = centers_.rows();
	const int dim = centers_.cols();
	assert(dim == 2);

	// K_cst = ∫ψ_k
	// K_lin = ∫∇x(ψ_k), ∫∇y(ψ_k)
	// K_mix = ∫y·∇x(ψ_k), ∫x·∇y(ψ_k)
	// K_sqr = ∫x·∇x(ψ_k), ∫y·∇y(ψ_k)
	Eigen::VectorXd K_cst = Eigen::VectorXd::Zero(num_kernels);
	Eigen::MatrixXd K_lin = Eigen::MatrixXd::Zero(num_kernels, dim);
	Eigen::MatrixXd K_mix = Eigen::MatrixXd::Zero(num_kernels, dim);
	Eigen::MatrixXd K_sqr = Eigen::MatrixXd::Zero(num_kernels, dim);
	for (int j = 0; j < num_kernels; ++j) {
		// ∫∇x(ψ_k)(p) = Σ_q (xq - xk) * 1/r * h'(r) * wq
		// - xq is the x coordinate of the q-th quadrature point
		// - wq is the q-th quadrature weight
		// - r is the distance from pq to the kernel center
		// - h is the RBF kernel (scalar function)
		for (int q = 0; q < quadr.points.rows(); ++q) {
			const RowVectorNd p = quadr.points.row(q) - centers_.row(j);
			const double r = p.norm();
			const RowVectorNd gradPhi = p * kernel_prime(is_volume(), r) / r * quadr.weights(q);
			K_cst(j) += kernel(is_volume(), r) * quadr.weights(q);
			K_lin.row(j) += gradPhi;
			K_mix(j,0) += quadr.points(q,1)*gradPhi(0);
			K_mix(j,1) += quadr.points(q,0)*gradPhi(1);
			K_sqr.row(j) += (quadr.points.row(q).array() * gradPhi.array()).matrix();
		}
	}

	// I_lin = ∫x, ∫y
	// I_mix = ∫xy
	// I_sqr = ∫x², ∫y²
	Eigen::RowVectorXd I_lin = (quadr.points.array().colwise() * quadr.weights.array()).colwise().sum();
	Eigen::RowVectorXd I_mix = (quadr.points.rowwise().prod().array() * quadr.weights.array()).colwise().sum();
	Eigen::RowVectorXd I_sqr = (quadr.points.array().square().colwise() * quadr.weights.array()).colwise().sum();
	double volume = quadr.weights.sum();

	// std::cout << I_lin << std::endl;
	// std::cout << I_mix << std::endl;
	// std::cout << I_sqr << std::endl;


	

	// Compute M
	Eigen::Matrix<double, 5, 5> M;
	M <<    volume,          0,          I_lin(1), 2*I_lin(0),          0,
	             0,     volume,          I_lin(0),          0, 2*I_lin(1),
	      I_lin(1),   I_lin(0), I_sqr(0)+I_sqr(1), 2*I_mix(0), 2*I_mix(0),
	    4*I_lin(0), 2*I_lin(1),        4*I_mix(0), 6*I_sqr(0), 2*I_sqr(1),
	    2*I_lin(0), 4*I_lin(1),        4*I_mix(0), 2*I_sqr(0), 6*I_sqr(1);
	Eigen::FullPivLU<Eigen::Matrix<double, 5, 5>> lu(M);
	assert(lu.isInvertible());


	ElementAssemblyValues ass_val;
	ass_val.has_parameterization=false;
	//linear linear
	{
		ass_val.basis_values.resize(5);
		//x
		ass_val.basis_values[0].val = quadr.points.col(0);
		ass_val.basis_values[0].grad = Eigen::MatrixXd(quadr.points.rows(), quadr.points.cols());
		ass_val.basis_values[0].grad.col(0).setOnes();
		ass_val.basis_values[0].grad.col(1).setZero();

		//y
		ass_val.basis_values[1].val = quadr.points.col(1);
		ass_val.basis_values[1].grad = Eigen::MatrixXd(quadr.points.rows(), quadr.points.cols());
		ass_val.basis_values[1].grad.col(0).setZero();
		ass_val.basis_values[1].grad.col(1).setOnes();

		//xy
		ass_val.basis_values[2].val = quadr.points.col(0).array() * quadr.points.col(1).array();
		ass_val.basis_values[2].grad = Eigen::MatrixXd(quadr.points.rows(), quadr.points.cols());
		ass_val.basis_values[2].grad.col(0) = quadr.points.col(1);
		ass_val.basis_values[2].grad.col(1) = quadr.points.col(0);

		//x^2
		ass_val.basis_values[3].val = quadr.points.col(0).array() * quadr.points.col(0).array();
		ass_val.basis_values[3].grad = Eigen::MatrixXd(quadr.points.rows(), quadr.points.cols());
		ass_val.basis_values[3].grad.col(0) = 2*quadr.points.col(0);
		ass_val.basis_values[3].grad.col(1).setZero();

		//y^2
		ass_val.basis_values[4].val = quadr.points.col(1).array() * quadr.points.col(1).array();
		ass_val.basis_values[4].grad = Eigen::MatrixXd(quadr.points.rows(), quadr.points.cols());
		ass_val.basis_values[4].grad.col(0).setZero();
		ass_val.basis_values[4].grad.col(1) = 2*quadr.points.col(1);

		for(size_t i = 0; i < ass_val.basis_values.size(); ++i)
		{
			ass_val.basis_values[i].grad_t_m = ass_val.basis_values[i].grad;
		}

		Laplacian assembler;


		DiffScalarBase::setVariableCount(quadr.points.cols());
		AutodiffHessianPt pt(1);
		Eigen::MatrixXd strong(quadr.points.rows(), 5);
		for(int i = 0; i < quadr.points.rows(); ++i)
		{
			//x
			pt(0) = AutodiffScalarHessian(0, quadr.points(i, 0));
			strong(i, 0) = assembler.compute_rhs(pt)(0);

			//y
			pt(0) = AutodiffScalarHessian(0, quadr.points(i, 1));
			strong(i, 1) = assembler.compute_rhs(pt)(0);

			//y
			pt(0) = AutodiffScalarHessian(0, quadr.points(i, 0)) * AutodiffScalarHessian(1, quadr.points(i, 1));
			strong(i, 2) = assembler.compute_rhs(pt)(0);

			//x^2
			pt(0) = AutodiffScalarHessian(0, quadr.points(i, 0)) * AutodiffScalarHessian(0, quadr.points(i, 0));
			strong(i, 3) = assembler.compute_rhs(pt)(0);

			//y^2
			pt(0) = AutodiffScalarHessian(0, quadr.points(i, 1)) * AutodiffScalarHessian(0, quadr.points(i, 1));
			strong(i, 4) = assembler.compute_rhs(pt)(0);
		}


		for(int i = 0; i < 5; ++i)
		{
			for(int j = 0; j < 5; ++j)
			{
				double automatic_val = assembler.assemble(ass_val, i, j, quadr.weights)(0);
				automatic_val += (strong.col(i).array() * ass_val.basis_values[j].val.array() * quadr.weights.array()).sum();
				std::cout<<"diff generic "<<i <<","<<j<<": "<<fabs( automatic_val - M(i,j))<<std::endl;
			}
		}

	}

	// show_matrix_stats(M);

	// Compute L
	L.resize(num_kernels + 1 + dim + dim*(dim+1)/2, num_kernels + 1);
	L.setZero();
	L.diagonal().setOnes();
	L.block(num_kernels + 1, 0, dim, num_kernels) = -K_lin.transpose();
	L.block(num_kernels + 1 + dim, 0, 1, num_kernels) = -K_mix.transpose().colwise().sum();
	L.block(num_kernels + 1 + dim + 1, 0, dim, num_kernels) = -2.0 * (K_sqr.colwise() + K_cst).transpose();
	L.bottomRightCorner(dim, 1).setConstant(-2.0 * volume);
	L.block(num_kernels + 1, 0, 5, num_kernels + 1) = lu.solve(L.block(num_kernels + 1, 0, 5, num_kernels + 1));
	// std::cout << L.bottomRightCorner(10, 10) << std::endl;

	// Compute t
	t.resize(L.rows(), num_bases);
	t.setZero();
	t.bottomRows(5) = local_basis_integral.transpose();
	t.bottomRows(5) = lu.solve(weights_.bottomRows(5));
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::compute_constraints_matrix_3d(
	const int num_bases,
	const Quadrature &quadr,
	const Eigen::MatrixXd &local_basis_integral,
	Eigen::MatrixXd &L,
	Eigen::MatrixXd &t) const
{
	const int num_kernels = centers_.rows();
	const int dim = centers_.cols();
	assert(dim == 3);
	assert(local_basis_integral.cols() == 9);

	// K_cst = ∫ψ_k
	// K_lin = ∫∇x(ψ_k), ∫∇y(ψ_k), ∫∇z(ψ_k)
	// K_mix = ∫(y·∇x(ψ_k)+x·∇y(ψ_k)), ∫(z·∇y(ψ_k)+y·∇z(ψ_k)), ∫(x·∇z(ψ_k)+z·∇x(ψ_k))
	// K_sqr = ∫x·∇x(ψ_k), ∫y·∇y(ψ_k), ∫z·∇z(ψ_k)
	Eigen::VectorXd K_cst = Eigen::VectorXd::Zero(num_kernels);
	Eigen::MatrixXd K_lin = Eigen::MatrixXd::Zero(num_kernels, dim);
	Eigen::MatrixXd K_mix = Eigen::MatrixXd::Zero(num_kernels, dim);
	Eigen::MatrixXd K_sqr = Eigen::MatrixXd::Zero(num_kernels, dim);
	for (int j = 0; j < num_kernels; ++j) {
		// ∫∇x(ψ_k)(p) = Σ_q (xq - xk) * 1/r * h'(r) * wq
		// - xq is the x coordinate of the q-th quadrature point
		// - wq is the q-th quadrature weight
		// - r is the distance from pq to the kernel center
		// - h is the RBF kernel (scalar function)
		for (int q = 0; q < quadr.points.rows(); ++q) {
			const RowVectorNd p = quadr.points.row(q) - centers_.row(j);
			const double r = p.norm();
			const RowVectorNd gradPhi = p * kernel_prime(is_volume(), r) / r * quadr.weights(q);
			K_cst(j) += kernel(is_volume(), r) * quadr.weights(q);
			K_lin.row(j) += gradPhi;
			for (int d = 0; d < dim; ++d) {
				K_mix(j,d) += quadr.points(q,(d+1)%dim)*gradPhi(d) + quadr.points(q,d)*gradPhi((d+1)%dim);
			}
			K_sqr.row(j) += (quadr.points.row(q).array() * gradPhi.array()).matrix();
		}
	}

	// I_lin = ∫x, ∫y, ∫z
	// I_sqr = ∫x², ∫y², ∫z²
	// I_mix = ∫xy, ∫yz, ∫zx
	Eigen::RowVectorXd I_lin = (quadr.points.array().colwise() * quadr.weights.array()).colwise().sum();
	Eigen::RowVectorXd I_sqr = (quadr.points.array().square().colwise() * quadr.weights.array()).colwise().sum();
	Eigen::RowVectorXd I_mix(3);
	I_mix(0) = (quadr.points.col(0).array() * quadr.points.col(1).array() * quadr.weights.array()).sum();
	I_mix(1) = (quadr.points.col(1).array() * quadr.points.col(2).array() * quadr.weights.array()).sum();
	I_mix(2) = (quadr.points.col(2).array() * quadr.points.col(0).array() * quadr.weights.array()).sum();
	double volume = quadr.weights.sum();

	// std::cout << I_lin << std::endl;
	// std::cout << I_mix << std::endl;
	// std::cout << I_sqr << std::endl;

	// Compute M
	Eigen::Matrix<double, 9, 9> M;
	M <<    volume,          0,          0,          I_lin(1),                 0,          I_lin(2), 2*I_lin(0),          0,          0,
	             0,     volume,          0,          I_lin(0),          I_lin(2),                 0,          0, 2*I_lin(1),          0,
	             0,          0,     volume,                 0,          I_lin(1),          I_lin(0),          0,          0, 2*I_lin(2),
	      I_lin(1),   I_lin(0),          0, I_sqr(0)+I_sqr(1),          I_mix(2),          I_mix(1), 2*I_mix(0), 2*I_mix(0),          0,
	             0,   I_lin(2),   I_lin(1),          I_mix(2), I_sqr(1)+I_sqr(2),          I_mix(0),          0, 2*I_mix(1), 2*I_mix(1),
	      I_lin(2),          0,   I_lin(0),          I_mix(1),          I_mix(0), I_sqr(2)+I_sqr(0), 2*I_mix(2),          0, 2*I_mix(2),
	    2*I_lin(0),          0,          0,        2*I_mix(0),                 0,        2*I_mix(2), 4*I_sqr(0),          0,          0,
	             0, 2*I_lin(1),          0,        2*I_mix(0),        2*I_mix(1),                 0,          0, 4*I_sqr(1),          0,
	             0,          0, 2*I_lin(2),                 0,        2*I_mix(1),        2*I_mix(2),          0,          0, 4*I_sqr(2);
	Eigen::Matrix<double, 1, 9> M_rhs;
	M_rhs.segment<3>(0) = I_lin;
	M_rhs.segment<3>(3) = I_mix;
	M_rhs.segment<3>(6) = I_sqr;
	// M_rhs << I_lin, I_mix, I_sqr;
	M.bottomRows(dim).rowwise() += 2.0 * M_rhs;
	Eigen::FullPivLU<Eigen::Matrix<double, 9, 9>> lu(M);
	assert(lu.isInvertible());

	// show_matrix_stats(M);

	// Compute L
	L.resize(num_kernels + 1 + dim + dim*(dim+1)/2, num_kernels + 1);
	L.setZero();
	L.diagonal().setOnes();
	L.block(num_kernels + 1, 0, dim, num_kernels) = -K_lin.transpose();
	L.block(num_kernels + 1 + dim, 0, dim, num_kernels) = -K_mix.transpose();
	L.block(num_kernels + 1 + dim + dim, 0, dim, num_kernels) = -2.0 * (K_sqr.colwise() + K_cst).transpose();
	L.bottomRightCorner(dim, 1).setConstant(-2.0 * volume);
	L.block(num_kernels + 1, 0, 9, num_kernels + 1) = lu.solve(L.block(num_kernels + 1, 0, 9, num_kernels + 1));
	// std::cout << L.bottomRightCorner(10, 10) << std::endl;

	// Compute t
	t.resize(L.rows(), num_bases);
	t.setZero();
	t.bottomRows(9) = local_basis_integral.transpose();
	t.bottomRows(9) = lu.solve(weights_.bottomRows(9));
}

// -----------------------------------------------------------------------------

void RBFWithQuadratic::compute_weights(const Eigen::MatrixXd &samples,
	const Eigen::MatrixXd &local_basis_integral, const Quadrature &quadr,
	Eigen::MatrixXd &rhs, bool with_constraints)
{
	#ifdef VERBOSE
	logger().trace("#kernel centers: {}", centers_.rows());
	logger().trace("#collocation points: {}", samples.rows());
	logger().trace("#quadrature points: {}", quadr.weights.size());
	logger().trace("#non-vanishing bases: {}", rhs.cols());
	#endif

	if (!with_constraints) {
		// Compute A
		Eigen::MatrixXd A;
		compute_kernels_matrix(samples, A);

		// Solve the system
		const int num_kernels = centers_.rows();
		logger().trace("-- Solving system of size {}x{}", num_kernels, num_kernels);
		weights_ = (A.transpose() * A).ldlt().solve(A.transpose() * rhs);
		logger().trace("-- Solved!");

		return;
	}

	const int num_bases = rhs.cols();

	// Compute A
	Eigen::MatrixXd A;
	compute_kernels_matrix(samples, A);

	// Compute L and t
	// Note that t is stored into `weights_` for memory efficiency reasons
	Eigen::MatrixXd L;
	if (is_volume()) {
		compute_constraints_matrix_3d(num_bases, quadr, local_basis_integral, L, weights_);
	} else {
		compute_constraints_matrix_2d(num_bases, quadr, local_basis_integral, L, weights_);
	}

	// Compute b = rhs - A t
	Eigen::MatrixXd b = rhs - A * weights_;

	// Solve the system
	#ifdef VERBOSE
	logger().trace("-- Solving system of size {}x{}", L.cols(), L.cols());
	#endif
	auto ldlt = (L.transpose() * A.transpose() * A * L).ldlt();
	if (ldlt.info() == Eigen::NumericalIssue) {
		logger().error("-- WARNING: Numerical issues when solving the harmonic least square.");
	}
	weights_ += L * ldlt.solve(L.transpose() * A.transpose() * b);
	#ifdef VERBOSE
	logger().trace("-- Solved!");
	#endif

	#ifdef VERBOSE
	logger().trace("-- Mean residual: {}", (A * weights_ - rhs).array().abs().colwise().maxCoeff().mean());
	#endif

#if 0
	Eigen::MatrixXd MM, x, dx, val;
	basis(0, quadr.points, val);
	grad(0, quadr.points, MM);
	int dim = (is_volume() ? 3 : 2);
	for (int d = 0; d < dim; ++d) {
		// basis(0, quadr.points, x);
		// auto asd = quadr.points;
		// asd.col(d).array() += 1e-7;
		// basis(0, asd, dx);
		// std::cout << (dx - x) / 1e-7 - MM.col(d) << std::endl;
		std::cout << (MM.col(d).array() * quadr.weights.array()).sum() - local_basis_integral(0, d) << std::endl;
		std::cout << ((
				MM.col((d+1)%dim).array() * quadr.points.col(d).array()
				+ MM.col(d).array() * quadr.points.col((d+1)%dim).array()
			) * quadr.weights.array()).sum() - local_basis_integral(0, (dim == 2 ? 2 : (dim+d) )) << std::endl;
		std::cout << 2.0 * (
				(quadr.points.col(d).array() * MM.col(d).array()
				+ val.array())
			* quadr.weights.array()
			).sum() - local_basis_integral(0, (dim == 2 ? (3 + d) : (dim+dim+d))) << std::endl;
	}
#endif
}
