// SPDX-License-Identifier: GPL-3.0-only
// Private ghost identity. Storage bytes, not shader floating-point values.
ByteAddressBuffer Input : register(t0);
RWByteAddressBuffer Leaves : register(u0);
cbuffer Layout : register(b0) {
    uint Length, LengthHigh, RowBytes, RowPitch;
    uint Offset, Count, HalfFloat, Initialize;
    uint FlagOffset;
};
static const uint K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
uint rotate(uint x,uint n) { return (x>>n)|(x<<(32-n)); }
uint logical_byte(uint j) {
    const uint address=Offset+(j/RowBytes)*RowPitch+j%RowBytes;
    // The owning buffer has a four-byte-rounded physical extent. Only this
    // selected byte enters SHA; final-word padding and row gaps never enter it.
    return (Input.Load(address&~3u)>>((address&3u)*8))&255u;
}
uint message_byte(uint at,uint index,uint bytes,uint padded) {
    if(at==0)return 0;
    if(at<9)return at<5?(Length>>((at-1)*8))&255u:(LengthHigh>>((at-5)*8))&255u;
    if(at<17)return at<13?(index>>((at-9)*8))&255u:0;
    if(at<21)return at==18?16u:0u; // LE32(4096)
    if(at<25)return (bytes>>((at-21)*8))&255u;
    if(at<25+bytes)return logical_byte(index*4096+at-25);
    if(at==25+bytes)return 128u;
    if(at>=padded-4)return (((25+bytes)*8)>>((padded-1-at)*8))&255u;
    return 0;
}
[numthreads(128,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(Initialize) {
        if(id.x==0)Leaves.Store(FlagOffset,0);
        return;
    }
    const uint index=id.x;
    if(index>=Count)return;
    const uint bytes=min(4096u,Length-index*4096);
    const uint padded=((25+bytes+9+63)/64)*64;
    uint h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for(uint block=0;block<padded;block+=64) {
        uint w[16];
        [unroll] for(uint j=0;j<16;++j) {
            const uint at=block+j*4;
            w[j]=(message_byte(at,index,bytes,padded)<<24)|
                (message_byte(at+1,index,bytes,padded)<<16)|
                (message_byte(at+2,index,bytes,padded)<<8)|message_byte(at+3,index,bytes,padded);
        }
        uint a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],v=h[7];
        for(uint round=0;round<64;++round) {
            const uint slot=round&15;
            if(round>=16) {
                const uint x=w[(round-15)&15],y=w[(round-2)&15];
                w[slot]+=(rotate(x,7)^rotate(x,18)^(x>>3))+w[(round-7)&15]+
                    (rotate(y,17)^rotate(y,19)^(y>>10));
            }
            const uint t1=v+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^((~e)&g))+K[round]+w[slot];
            const uint t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            v=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=v;
    }
    [unroll] for(uint word=0;word<8;++word) {
        const uint value=h[word];
        Leaves.Store(index*32+word*4,((value&255)<<24)|((value&0xff00)<<8)|
            ((value>>8)&0xff00)|(value>>24));
    }
    if(HalfFloat) {
        uint invalid=0;
        for(uint j=0;j+1<bytes;j+=2) {
            const uint bits=logical_byte(index*4096+j)|(logical_byte(index*4096+j+1)<<8);
            invalid|=uint((bits&0x7c00)==0x7c00);
        }
        if(invalid) {
            uint prior;
            Leaves.InterlockedOr(FlagOffset,invalid,prior);
        }
    }
}
