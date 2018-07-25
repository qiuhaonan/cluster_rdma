
void wire_gid_to_gid(const char* wgid,union ibv_gid* gid)
{
	char tmp[9];
	uint32_t v32;
	int i=0;
	for(tmp[8]=0;i<4;++i)
	{
		memcpy(tmp,wgid+i*8,8);
		sscanf(tmp,"%x",&v32);
		*(uint32_t*)(&gid->raw[i*4])=ntohl(v32);
	}
}

void gid_to_wire_gid(const union ibv_gid* gid,char wgid[])
{
	int i=0;
	for(;i<4;++i)
	{
		sprintf(&wgid[i*8],"%08x",htonl(*(uint32_t*)(gid->raw+i*4)));
	}
}
