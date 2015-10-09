#include "Mesh.h"
#include "unique_name.h"
#include "quattrans2affine.h"
#include "list_affine_to_stack.h"

// Functions
#include <igl/quat_to_mat.h>
#include <igl/opengl2/draw_mesh.h>
#include <igl/per_face_normals.h>
#include <igl/per_vertex_normals.h>
#include <igl/per_corner_normals.h>
#include <igl/opengl2/sort_triangles.h>
#include <igl/dqs.h>

// Constants
#include <igl/material_colors.h>

#include <iostream>

Mesh::Mesh():
  Pickle(),
  V(0,3),F(0,3),VN(0,3),FN(0,3),CN(0,3),name(unique_name()),
  normal_type(igl::PER_VERTEX_NORMALS),
  invert_orientation(false),
  display_list_compiled(false),
  dl_id(0),
  scale(1),
  shift(),
  y_boost(0),
  rotation(1,0,0,0),
  is_hover(false),
  is_selected(false),
  is_down(false)
{
  using namespace igl;
  using namespace std;
  shift(0) = shift(1) = shift(2) = 0;
  copy(GOLD_DIFFUSE,GOLD_DIFFUSE+4,color);
}

void Mesh::compute_scale_and_shift()
{
  using namespace std;
  if(V.size() == 0)
  {
    return;
  }

  assert(V.cols() == 3);
  //shift[0] = -V.col(0).mean();
  //shift[1] = -V.col(1).mean();
  //shift[2] = -V.col(2).mean();
  //scale = 2.0/
  //  max(V.col(0).maxCoeff() - V.col(0).minCoeff(),
  //    max(V.col(1).maxCoeff() - V.col(1).minCoeff(),
  //      V.col(2).maxCoeff() - V.col(2).minCoeff()));
  // Tightly fit bounding box in unit sphere
  scale = 2.0/(V.colwise().maxCoeff() - V.colwise().minCoeff()).norm();
  shift = -0.5*(V.colwise().maxCoeff() + V.colwise().minCoeff());
}

void Mesh::push_matrix() const
{
  using namespace igl;
  using namespace Eigen;
  glPushMatrix();

  Affine3d t;
  t.setIdentity();
  t.rotate(rotation);
  glMultMatrixd(t.matrix().data());
  glScaled(scale,scale,scale);
  glTranslated(shift(0),shift(1),shift(2));
}

Eigen::MatrixXd Mesh::as_drawn() const
{
  using namespace Eigen;
  MatrixXd VD = V;
  VD.col(0).array() += shift(0);
  VD.col(1).array() += shift(1);
  VD.col(2).array() += shift(2);
  VD *= scale;
  VD = VD * rotation.matrix().transpose();
  return VD;
}

void Mesh::relative_to_mesh(Eigen::MatrixXd & C) const
{
  if(C.size() == 0)
  {
    return;
  }
  C = C * rotation.matrix();
  C /= scale;
  C.col(0).array() -= shift(0);
  C.col(1).array() -= shift(1);
  C.col(2).array() -= shift(2);
}

void Mesh::unrelative_to_mesh(Eigen::MatrixXd & C) const
{
  if(C.size() == 0)
  {
    return;
  }
  C.col(0).array() += shift(0);
  C.col(1).array() += shift(1);
  C.col(2).array() += shift(2);
  C *= scale;
  C = C * rotation.conjugate().matrix();
}

void Mesh::pop_matrix() const
{
  glPopMatrix();
}

void Mesh::draw()
{
  using namespace igl;
  using namespace igl::opengl2;
  using namespace std;
  Eigen::MatrixXd * N;
  switch(normal_type)
  {
    case PER_VERTEX_NORMALS:
      N = &VN;
      break;
    case PER_FACE_NORMALS:
      N = &FN;
      break;
    case PER_CORNER_NORMALS:
      N = &CN;
      break;
    default:
      N = &VN;
  }

  GLint old_ff;
  if(invert_orientation)
  {
    glGetIntegerv(GL_FRONT_FACE,&old_ff);
    glFrontFace(GL_CW);
    *N *= -1.0;
  }
  draw_mesh(dgetV(),dgetF(),*N);
  if(invert_orientation)
  {
    *N *= -1.0;
    glFrontFace(old_ff);
  }
}


void Mesh::draw_and_cache()
{
  using namespace igl;
  using namespace std;
  push_matrix();
  if(!display_list_compiled)
  {
    if(glIsList(dl_id))
    {
      glDeleteLists(dl_id,1);
    }
    dl_id = glGenLists(1);
    glNewList(dl_id,GL_COMPILE);
    draw();
    glEndList();
    display_list_compiled = true;
  }
  glCallList(dl_id);
  pop_matrix();
}

void Mesh::sort()
{
  using namespace igl;
  using namespace igl::opengl2;
  using namespace std;
  using namespace Eigen;
  MatrixXi FF;
  VectorXi I;
  sort_triangles(getV(),getF(),FF,I);
  dgetF() = FF;
  per_face_normals(getV(),getF(),dgetFN());
  per_vertex_normals(getV(),getF(),dgetVN());
  per_corner_normals(getV(),getF(),getFN(),30.0,dgetCN());
}

bool Mesh::down(const int /*mouse_x*/, const int /*mouse_y*/)
{
  return is_down;
}

bool Mesh::drag(const int /*mouse_x*/, const int /*mouse_y*/)
{
  return is_down;
}

bool Mesh::up(const int /*mouse_x*/, const int /*mouse_y*/)
{
  return is_down;
}

bool Mesh::set_invert_orientation(const bool v)
{
  bool old=invert_orientation;
  if(v != invert_orientation)
  {
    display_list_compiled = false;
    invert_orientation = v;
  }
  return old;
}

bool Mesh::get_invert_orientation() const
{
  return invert_orientation;
}

bool Mesh::deformed_copy(
  const SkinningMethod & skinning_method,
  const Eigen::MatrixXd & W,
  const std::vector<
    Eigen::Quaterniond,Eigen::aligned_allocator<Eigen::Quaterniond> > & vQ,
  const std::vector<Eigen::Vector3d> & vT,
  const Eigen::MatrixXd & M,
  Mesh & m_def
    ) const
{
  using namespace Eigen;
  using namespace igl;
  using namespace std;

  // Update faces for first time and for sorts
  m_def.dget_normal_type() = get_normal_type();
  m_def.set_invert_orientation(get_invert_orientation());
  m_def.dgetF() = getF();
  // Do **not** copy shift, scale, camera as those are baked into lbs,dqs
  // via as_drawn()
  switch(skinning_method)
  {
    case SKINNING_METHOD_DQS:
      {
        dqs(as_drawn(),W,vQ,vT,m_def.dgetV());
        break;
      }
    case SKINNING_METHOD_LBS:
      {
        vector<Affine3d,aligned_allocator<Affine3d> > vA;
        quattrans2affine(vQ,vT,vA);
        MatrixXd T;
        list_affine_to_stack(vA,T);
        m_def.dgetV() = M * T;
        break;
      }
    default:
      {
        assert(false && "Unknown skinning method");
        return false;
      }
  }

  // Shift so mins are the same: also a foot on the ground
  {
    const double dy = as_drawn().col(1).minCoeff() - m_def.dgetV().col(1).minCoeff();
    m_def.y_boost = dy;
  }


  switch(get_normal_type())
  {
    case PER_FACE_NORMALS:
      per_face_normals(  m_def.dgetV(),m_def.dgetF(),m_def.dgetFN());
      break;
    case PER_VERTEX_NORMALS:
      per_face_normals(  m_def.dgetV(),m_def.dgetF(),m_def.dgetFN());
      per_vertex_normals(m_def.dgetV(),m_def.dgetF(),m_def.dgetFN(),m_def.dgetVN());
      break;
    case PER_CORNER_NORMALS:
      per_face_normals(  m_def.dgetV(),m_def.dgetF(),m_def.dgetFN());
      per_corner_normals(m_def.dgetV(),m_def.dgetF(),m_def.dgetFN(),10.0,m_def.dgetCN());
      break;
  }
  return true;
}
