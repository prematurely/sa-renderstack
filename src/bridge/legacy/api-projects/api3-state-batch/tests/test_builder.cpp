#include "api3_state_batch.hpp"
#include <cassert>
int main(){ api3::StateBatchBuilder b; float a[4]={1,2,3,4}, c[4]={5,6,7,8}; assert(b.add_vertex_float(0,a).has_value()); assert(b.add_vertex_float(1,c).has_value()); auto v=b.view(); assert(v && v->VertexFloatRangeCount==1 && v->VertexFloatRanges[0].RegisterCount==2); assert(!b.add_vertex_float(255,std::span<const float>(a,4)).has_value()); return 0; }
