#pragma once

#include "AssemblerUtils.hpp"

#include <polyfem/basis/ElementBases.hpp>
#include <polyfem/mesh/LocalBoundary.hpp>
#include <polyfem/mesh/Mesh.hpp>

#include <polyfem/Common.hpp>

#include <vector>
#include <Eigen/Dense>
#include <memory>

namespace polyfem
{
	namespace assembler
	{
		class Problem
		{
		public:
			Problem(const std::string &name);
			virtual ~Problem() {}

			virtual void init(const mesh::Mesh &mesh) {}
			inline const std::string &name() const { return name_; }

			virtual bool is_scalar() const = 0;

			virtual void rhs(const assembler::AssemblerUtils &assembler, const std::string &formulation, const Eigen::MatrixXd &pts, const double t, Eigen::MatrixXd &val) const = 0;
			virtual bool is_rhs_zero() const = 0;

			virtual void bc(const mesh::Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &uv, const Eigen::MatrixXd &pts, const double t, Eigen::MatrixXd &val) const = 0;
			virtual void neumann_bc(const mesh::Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &uv, const Eigen::MatrixXd &pts, const Eigen::MatrixXd &normals, const double t, Eigen::MatrixXd &val) const {}

			virtual bool has_exact_sol() const = 0;
			virtual void exact(const Eigen::MatrixXd &pts, const double t, Eigen::MatrixXd &val) const {};
			virtual void exact_grad(const Eigen::MatrixXd &pts, const double t, Eigen::MatrixXd &val) const {};

			virtual void clear() {}

			virtual bool is_time_dependent() const { return false; }
			virtual bool is_constant_in_time() const { return true; }

			virtual void initial_solution(const mesh::Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const {}
			virtual void initial_velocity(const mesh::Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const {}
			virtual void initial_acceleration(const mesh::Mesh &mesh, const Eigen::MatrixXi &global_ids, const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const {}
			virtual void initial_density(const Eigen::MatrixXd &pts, Eigen::MatrixXd &val) const {}

			virtual void set_parameters(const json &params) {}

			virtual bool might_have_no_dirichlet() { return false; }
			virtual bool is_dimension_dirichet(const int tag, const int dim) const { return true; }
			virtual bool all_dimensions_dirichlet() const { return true; } //here for efficiency reasons

			void setup_bc(const mesh::Mesh &mesh, const std::vector<basis::ElementBases> &bases, const std::vector<basis::ElementBases> &pressure_bases, std::vector<mesh::LocalBoundary> &local_boundary, std::vector<int> &boundary_nodes, std::vector<mesh::LocalBoundary> &local_neumann_boundary, std::vector<int> &pressure_boundary_nodes);

		protected:
			std::vector<int> boundary_ids_;
			std::vector<int> neumann_boundary_ids_;
			std::vector<int> pressure_boundary_ids_;
			std::vector<int> splitting_pressure_boundary_ids_;

		private:
			std::string name_;
		};
	} // namespace assembler
} // namespace polyfem
