#ifndef __VCGFOREMBREE_H
#define __VCGFOREMBREE_H

#include <iostream>
#include <vcg/complex/complex.h>

#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <wrap/callback.h>
#include <embree3/rtcore.h>
#include <vcg/math/gen_normal.h>
#include <limits>
#include <math.h>
#include <time.h>
#include <omp.h>

class MyVertex; class MyEdge; class MyFace;
struct MyUsedTypes : public vcg::UsedTypes<vcg::Use<MyVertex>   ::AsVertexType,
                                           vcg::Use<MyEdge>     ::AsEdgeType,
                                           vcg::Use<MyFace>     ::AsFaceType>{};

class MyVertex  : public vcg::Vertex< MyUsedTypes, vcg::vertex::Coord3f, vcg::vertex::Normal3f, vcg::vertex::BitFlags, vcg::vertex::VFAdj, vcg::vertex::Qualityf, vcg::vertex::Color4b>{};
class MyFace    : public vcg::Face<   MyUsedTypes, vcg::face::FFAdj ,vcg::face::VFAdj, vcg::face::Normal3f,  vcg::face::VertexRef, vcg::face::BitFlags, vcg::face::Color4b, vcg::face::Qualityf> {};
class MyEdge    : public vcg::Edge<   MyUsedTypes> {};

class MyMesh    : public vcg::tri::TriMesh< std::vector<MyVertex>, std::vector<MyFace> , std::vector<MyEdge>  > {};

using namespace vcg;
using namespace std;


/*
    @Author: Paolo Fasano
    This class aims to integrate intel embree3 with the vcglib giving some basic methods that can be used to build more complex features.
*/
namespace vcg{
    template <class MeshType>
    class EmbreeAdaptor{

        RTCDevice device = rtcNewDevice(NULL);
        RTCScene scene = rtcNewScene(device);
        RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);       
        int threads;  

        public: 
         EmbreeAdaptor(MeshType &m, int nOfthreads){
                threads = nOfthreads;
                loadVCGMeshInScene(m);  
            }

        /*
        @Author: Paolo Fasano
        @Parameter: Point3f rayDirection, this variable indicates the direction the rays are shoot towards
        @Description: Given a mesh (loaded in the constructor using the loadVCGMeshInScene method) foreach face 
            the barycenter is found and shoots a single ray starting from the barycenter. If the ray intersect with
            something the face color is set to black else is set to white.  
        */
        public:       
         void selectVisibleFaces(MeshType &m, Point3f rayDirection){
            
            RTCRayHit rayhit;           

            for(int i = 0;i<m.FN(); i++)
            {                    
                Point3f b = vcg::Barycenter(m.face[i]);
                std::vector<Point3f> unifDirVec;
                Point3f dir = rayDirection;

                rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];

                rayhit.ray.tnear  = 0.5f;
                rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                
                RTCIntersectContext context;
                rtcInitIntersectContext(&context);

                rtcIntersect1(scene, &context, &rayhit);

                if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID)            
                    m.face[i].C() = Color4b::Black;
                else                      
                    m.face[i].C() = Color4b::White;       
                           
            }
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);
           
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh
        @Description: Given a mesh this method apply some preprocessing over it using standard vcglib methods. 
            Than the mesh is loaded as a new embree geometry inside a new embree scene. The new embree variables 
            are global to the class in order to be used with the other methods.   
        */
        public:       
         void loadVCGMeshInScene(MeshType &m){
            //a little mesh preprocessing before adding it to a RTCScene          
            tri::RequirePerVertexNormal(m);
            tri::UpdateNormal<MyMesh>::PerVertexNormalized(m);
            tri::UpdateNormal<MyMesh>::PerFaceNormalized(m);
            tri::UpdateBounding<MyMesh>::Box(m);
            tri::UpdateFlags<MyMesh>::FaceClearV(m);
                       
            float* vb = (float*) rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3*sizeof(float), m.VN());
            for (int i = 0;i<m.VN(); i++){
                vb[i*3]=m.vert[i].P()[0];
                vb[i*3+1]=m.vert[i].P()[1];
                vb[i*3+2]=m.vert[i].P()[2];
            }

            unsigned* ib = (unsigned*) rtcSetNewGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3*sizeof(unsigned), m.FN());
            for(int i = 0;i<m.FN(); i++){
                ib[i*3] = tri::Index(m,m.face[i].V(0));
                ib[i*3+1] = tri::Index(m,m.face[i].V(1));
                ib[i*3+2] = tri::Index(m,m.face[i].V(2));
            }

            rtcCommitGeometry(geometry);
            rtcAttachGeometry(scene, geometry);
            rtcReleaseGeometry(geometry);
            rtcCommitScene(scene);                     
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: int nRay, this parameter indicates the number of rays that must be generated and shoot. 
        @Description: Given a mesh for each face from the barycenter this method shoots n rays towards a generated direction(to infinity).
            If the ray direction is not pointing inside than the ray is actually shoot. 
            If the ray intersect something than the face quality of the mesh is updated with the normal of the fica multiplied by the direction.       
        */
        public:
         void computeAmbientOcclusion(MeshType &inputM, int nRay){
            std::vector<Point3f> unifDirVec;
            GenNormal<float>::Fibonacci(nRay,unifDirVec);           
            computeAmbientOcclusion(inputM, unifDirVec);
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: std::vector<Point3f> unifDirVec, this parameter is a vector of direction specified by the user. 
        @Description: Given a mesh for each face from the barycenter this method shoots n rays towards some user generated directions(to infinity).
            If the ray direction is not pointing inside than the ray is actually shoot. 
            If the ray intersect something than the face quality of the mesh is updated with the normal of the fica multiplied by the direction.       
        */
        public:
         void computeAmbientOcclusion(MeshType &inputM, std::vector<Point3f> unifDirVec){
            tri::UpdateQuality<MeshType>::FaceConstant(inputM,0);
            #pragma omp parallel shared(inputM) 
            {
                #pragma omp for 
                for(int i = 0;i<inputM.FN(); i++)
                {       
                    RTCRayHit rayhit;     
                    Point3f b = vcg::Barycenter(inputM.face[i]);
                    rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];  
                    rayhit.ray.tnear  = 0.00001f;
                    
                    for(int r = 0; r<unifDirVec.size(); r++){
                        Point3f dir = unifDirVec.at(r);                       
                        float scalarP = inputM.face[i].N()*dir;

                        if(scalarP>0){
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;  

                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) 
                                inputM.face[i].Q()+=scalarP;
                                        
                        }
                    }
                }
            }
            tri::UpdateColor<MeshType>::PerFaceQualityGray(inputM);
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: int nRay, this parameter indicates the number of rays that must be generated and shoot. 
        @Parameter: float Tau, the grater this value is the grater the influence of the rays that intersect with some face
        @Description: Given a mesh for each face from the barycenter this method shoots n rays towards a generated direction(to infinity).
            If the ray direction is not pointing inside than the ray is actually shoot. 
            If the ray intersect something than the face quality of the mesh is updated with the normal of the fica multiplied by the direction;
            else, if there are no hits, the face get updated of 1-distanceHit^tau       
        */
        public:
         void computeObscurance(MeshType &inputM, int nRay, float tau){          
            std::vector<Point3f> unifDirVec;
                GenNormal<float>::Fibonacci(nRay,unifDirVec);

           computeObscurance(inputM, nRay, unifDirVec, tau);
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: std::vector<Point3f> unifDirVec, this parameter is a vector of direction specified by the user. 
        @Parameter: float Tau, the grater this value is the grater the influence of the rays that intersect with some face
        @Description: Given a mesh for each face from the barycenter this method shoots n rays towards a generated direction(to infinity).
            If the ray direction is not pointing inside than the ray is actually shoot. 
            If the ray intersect something than the face quality of the mesh is updated with the normal of the fica multiplied by the direction;
            else, if there are no hits, the face get updated of 1-distanceHit^tau       
        */
        public:
         void computeObscurance(MeshType &inputM, std::vector<Point3f> unifDirVec, float tau){
            tri::UpdateQuality<MeshType>::FaceConstant(inputM,0);           

            #pragma omp parallel 
            {
                #pragma omp for
                for(int i = 0;i<inputM.FN(); i++)
                {           
                    RTCRayHit rayhit; 
                    Point3f b = vcg::Barycenter(inputM.face[i]);
                    rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];  
                    rayhit.ray.tnear  = 0.00001f;
                    
                    for(int r = 0; r<unifDirVec.size(); r++){
                        Point3f dir = unifDirVec.at(r);                       
                        float scalarP = inputM.face[i].N()*dir;

                        if(scalarP>0){
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID)                             
                                inputM.face[i].Q()+=scalarP;                             
                            else         
                                inputM.face[i].Q()+=(1-powf(rayhit.ray.tfar,tau));
                                                          
                        }
                    }
                }
            }
            tri::UpdateColor<MeshType>::PerFaceQualityGray(inputM);
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);
        }

        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: int nRay, this parameter indicates the number of rays that must be generated and shoot. 
        @Parameter: float degree, this variable represents the angle of the cone for which we consider a point as a valid direction
        @Description: Given a mesh for each face from the barycenter this method shoots n rays towards a generated direction(to infinity).
            If the ray direction is not pointing inside and the angle is no more than degree, than the ray is actually shoot. 
            If the ray intersect something than the face quality of the mesh is updated with the distance between the barycenter and the hit.
            The face quality value is than updated dividing it by the number of hits.       
        */
        public:
         void computeSDF(MeshType &inputM, int nRay, float degree){           
            float omega = (2 * M_PI * (1-cos(degree)))/ (4.0*M_PI );
            //cout<<nRay/omega<<endl;
            std::vector<Point3f> unifDirVec;
            GenNormal<float>::Fibonacci(nRay/omega,unifDirVec);
            tri::UpdateQuality<MeshType>::FaceConstant(inputM,0);

            #pragma omp parallel 
            {
                #pragma omp for
                for(int i = 0;i<inputM.FN(); i++)
                {   
                    RTCRayHit rayhit;         
                    Point3f b = vcg::Barycenter(inputM.face[i]);
                    rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];  
                    rayhit.ray.tnear  = 1e-4;   
                    
                    int control = 0;
                    int nHits=0;
                    for(int r = 0; r<unifDirVec.size(); r++){
                        Point3f dir = unifDirVec.at(r);                        
                        float scalarP = inputM.face[i].N()*dir;

                        if(scalarP<0 && (b.dot(unifDirVec.at(r)) >= cos(degree) )){
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID; 

                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) 
                            {
                                nHits++;
                                inputM.face[i].Q()+=rayhit.ray.tfar;
                            } 

                            control++;
                                
                        }

                        if(control==nRay)
                            r = unifDirVec.size();
                    }

                    inputM.face[i].Q()/=nHits;
                }
            }
            tri::UpdateColor<MeshType>::PerFaceQualityRamp(inputM);
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);
        }


        /*
        @Author: Paolo Fasano
        @Parameter: MeshType &m, this variable indicates reference to a mesh.
        @Parameter: int nRay, this parameter indicates the number of rays that must be generated and shoot. 
        @Description: Given a mesh for each face, for each ray, it detects all the intersections with all the facets
            (i.e., without stopping at the first intersection), and accumulates the number.  
            The rays are shoot two times each, one inside and one outside the mesh.
            After shooting all the rays, the facet is flipped if frontHit>backHit.

            For more informations read:  
            Kenshi Takayama, Alec Jacobson, Ladislav Kavan, and Olga Sorkine-Hornung, A Simple Method for Correcting Facet Orientations in Polygon Meshes Based on Ray Casting, Journal of Computer Graphics Techniques (JCGT), vol. 3, no. 4, 53-63, 2014
            Available online http://jcgt.org/published/0003/04/02/      
        */
        public:
         void computeNormalAnalysis(MeshType &inputM, int nRay){
            tri::UpdateQuality<MeshType>::FaceConstant(inputM,0);
            std::vector<Point3f> unifDirVec;
            GenNormal<float>::Fibonacci(nRay,unifDirVec);

            #pragma omp parallel 
            {
                double start; 
                double end; 
                start = omp_get_wtime(); 
                #pragma omp for
                for(int i = 0;i<inputM.FN(); i++)
                {           
                    RTCRayHit rayhit; 
                    Point3f b = vcg::Barycenter(inputM.face[i]);
                    rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];  
                    
                    
                    int frontHit = 0;
                    int backHit = 0;

                    for(int r = 0; r<unifDirVec.size(); r++){
                        Point3f dir = unifDirVec.at(r);                       
                        float scalarP = inputM.face[i].N()*dir;

                        if(scalarP>0){
                            rayhit.ray.tnear  = 1e-4f;
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                frontHit++;
                                Point3f p = b+dir*rayhit.ray.tfar;
                                frontHit += findInterceptNumber(p*2);
                            }                            
                             
                            rayhit.ray.tnear  = -1e-4f;                              
                            rayhit.ray.dir_x  = -dir[0]; rayhit.ray.dir_y = -dir[1]; rayhit.ray.dir_z = -dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                backHit++;
                                Point3f p = b+dir*rayhit.ray.tfar;
                                backHit += findInterceptNumber(-p*2);
                            }
                                                         
                        }
                        else{
                            rayhit.ray.tnear  = -1e-4f;
                            rayhit.ray.dir_x  = -dir[0]; rayhit.ray.dir_y = -dir[1]; rayhit.ray.dir_z = -dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                frontHit++;
                                Point3f p = b+dir*rayhit.ray.tfar;
                                frontHit += findInterceptNumber(p*2);
                            }                            
                             
                            rayhit.ray.tnear  = 1e-4f;                              
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                backHit++;
                                Point3f p = b+dir*rayhit.ray.tfar;
                                backHit += findInterceptNumber(-p*2);
                            }

                        }
                        
                        
                    }

                    if(frontHit>backHit)
                        inputM.face[i].N() *= -1;//cout<<i<<" should be flipped"<<endl;
                    
                }
                cout<<"time "<< (omp_get_wtime() - start)/60 <<endl;
            }
            tri::UpdateColor<MeshType>::PerFaceQualityGray(inputM);
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);
         }

        
        /*
        @Author: Paolo Fasano
        @Parameter: Point3f origin, the origin point to search for intersections to 
        @Description: given an origin point this methos counts how many intersection there are starting from there 
            (only the number not the positions coordinates)     
        */
        public: 
         int findInterceptNumber(Point3f origin){         
            RTCRayHit rayhit;
            int totInterception = 0;

            Point3f b = origin;
            Point3f dir = origin*2;

            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
            rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];

            rayhit.ray.tnear  = 0.5f;
            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            
            RTCIntersectContext context;
            rtcInitIntersectContext(&context);

            rtcIntersect1(scene, &context, &rayhit);

            if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID){
                Point3f p = b+dir*rayhit.ray.tfar;
                totInterception+=1+findInterceptNumber(p);
            }                          
            else{
                return totInterception;
            }                      
                                   

         }


        public:
         std::vector<Point3f> AOBentNormal(MeshType &inputM, int nRay){
            std::vector<Point3f> unifDirVec;
            std::vector<Point3f> bentNormalDir;            
            GenNormal<float>::Fibonacci(nRay,unifDirVec);
            tri::UpdateQuality<MeshType>::FaceConstant(inputM,0);

            #pragma omp parallel 
            {
                #pragma omp for
                for(int i = 0;i<inputM.FN(); i++)
                {       
                    RTCRayHit rayhit;     
                    Point3f b = vcg::Barycenter(inputM.face[i]);
                    rayhit.ray.org_x  = b[0]; rayhit.ray.org_y = b[1]; rayhit.ray.org_z = b[2];  
                    rayhit.ray.tnear  = 1e-4;
                    
                    Point3f bN;
                    int accRays=0;
                    for(int r = 0; r<nRay; r++){
                        Point3f dir = unifDirVec.at(r);
                        
                        float scalarP = inputM.face[i].N()*dir;

                        if(scalarP>0){
                            rayhit.ray.dir_x  = dir[0]; rayhit.ray.dir_y = dir[1]; rayhit.ray.dir_z = dir[2];
                            rayhit.ray.tfar   = std::numeric_limits<float>::infinity();
                            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;   
                            RTCIntersectContext context;
                            rtcInitIntersectContext(&context);

                            rtcIntersect1(scene, &context, &rayhit);

                            if (rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) 
                            {                            
                                bN+=dir;
                                accRays++; 
                            } 
                                
                        }
                    }               
                    bentNormalDir.push_back(bN/accRays);
                
                    //inputM.face[i].Q()/=accRays;//inputM.face[i].N()*bentNormalDir;
                }
            }
            //tri::UpdateColor<MeshType>::PerFaceQualityGray(inputM);
            rtcReleaseScene(scene);
            rtcReleaseDevice(device);

            return bentNormalDir;
        }    

    };
}
#endif