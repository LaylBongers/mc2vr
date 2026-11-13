// An #include for tools/test/test_hlsl.cpp (include resolution and the depfile).
float3 clamp_hdr(float3 v) { return min(max(v, 1e-5), 30000); }
