"""Generate d3d11_bridge_tables.inc + d3d11_bridge_dxgi_tables.inc:
per-interface guest vtables (method name + exact 32-bit arg count incl. this),
mapping implemented methods to handlers and the rest to noop_<N> stubs."""
import re, sys
H = open(sys.argv[1]).read() + open(sys.argv[2]).read()

def vtbl(iface):
    m = re.search(r'typedef struct '+iface+r'Vtbl\s*\{(.*?)\n\}\s*'+iface+r'Vtbl;', H, re.S)
    if not m: return None
    body = m.group(1)
    # split into method entries: "RET (STDMETHODCALLTYPE *Name)( params );"
    out = []
    for mm in re.finditer(r'\(\s*STDMETHODCALLTYPE\s*\*\s*(\w+)\s*\)\s*\((.*?)\);', body, re.S):
        name = mm.group(1); params = mm.group(2)
        # count params: split by top-level commas
        p = params.strip()
        # remove the leading This param
        # count commas at depth 0
        depth=0; n=0; cur=''
        parts=[]
        for ch in p:
            if ch in '([{': depth+=1
            elif ch in ')]}': depth-=1
            if ch==',' and depth==0:
                parts.append(cur); cur=''
            else: cur+=ch
        if cur.strip(): parts.append(cur)
        # parts includes This; count 32-bit slots. In the guest every pointer/UINT/enum/float/HWND = 1 slot.
        # 64-bit value params don't occur in these interfaces (SIZE_T is passed as pointer out or UINT). GetRequiresFlags returns UINT64 (edx:eax) - no param issue.
        slots = len([x for x in parts if x.strip()])
        out.append((name, slots))  # slots includes This
    return out

# handler map: method -> C handler name. Unmapped -> noop_<argc>.
DEVCHILD = {'QueryInterface':'m_QueryInterface','AddRef':'m_AddRef','Release':'m_Release','GetDevice':'m_GetDevice',
 'GetPrivateData':'m_GetPrivateData','SetPrivateData':'m_SetPrivateData','SetPrivateDataInterface':'m_SetPrivateDataInterface'}
RES = dict(DEVCHILD, **{'GetType':'res_GetType','SetEvictionPriority':'res_SetEvictionPriority','GetEvictionPriority':'res_GetEvictionPriority'})
VIEW = dict(DEVCHILD, **{'GetResource':'view_GetResource'})
DXGIOBJ = {'QueryInterface':'m_QueryInterface','AddRef':'m_AddRef','Release':'m_Release',
 'SetPrivateData':'obj_SetPrivateData','SetPrivateDataInterface':'obj_SetPrivateDataInterface','GetPrivateData':'obj_GetPrivateData','GetParent':'fac_GetParent'}

MAP = {
 'ID3D11Buffer': dict(RES, GetDesc='buf_GetDesc'),
 'ID3D11Texture1D': dict(RES, GetDesc='t1d_GetDesc'),
 'ID3D11Texture2D': dict(RES, GetDesc='t2d_GetDesc'),
 'ID3D11Texture3D': dict(RES, GetDesc='t3d_GetDesc'),
 'ID3D11RenderTargetView': dict(VIEW, GetDesc='rtv_GetDesc'),
 'ID3D11DepthStencilView': dict(VIEW, GetDesc='dsv_GetDesc'),
 'ID3D11ShaderResourceView': dict(VIEW, GetDesc='srv_GetDesc'),
 'ID3D11UnorderedAccessView': dict(VIEW, GetDesc='uav_GetDesc'),
 'ID3D11InputLayout': dict(DEVCHILD),
 'ID3D11VertexShader': dict(DEVCHILD), 'ID3D11PixelShader': dict(DEVCHILD), 'ID3D11GeometryShader': dict(DEVCHILD),
 'ID3D11HullShader': dict(DEVCHILD), 'ID3D11DomainShader': dict(DEVCHILD), 'ID3D11ComputeShader': dict(DEVCHILD),
 'ID3D11SamplerState': dict(DEVCHILD, GetDesc='smp_GetDesc'),
 'ID3D11BlendState': dict(DEVCHILD, GetDesc='bl_GetDesc'),
 'ID3D11DepthStencilState': dict(DEVCHILD, GetDesc='dss_GetDesc'),
 'ID3D11RasterizerState': dict(DEVCHILD, GetDesc='rs_GetDesc'),
 'ID3D11Query': dict(DEVCHILD, GetDataSize='q_GetDataSize', GetDesc='q_GetDesc'),
 'ID3D11Device': {m:'dev_'+m for m in ['CreateBuffer','CreateTexture1D','CreateTexture2D','CreateTexture3D',
     'CreateShaderResourceView','CreateUnorderedAccessView','CreateRenderTargetView','CreateDepthStencilView',
     'CreateInputLayout','CreateVertexShader','CreateGeometryShader','CreateGeometryShaderWithStreamOutput',
     'CreatePixelShader','CreateHullShader','CreateDomainShader','CreateComputeShader','CreateClassLinkage',
     'CreateBlendState','CreateDepthStencilState','CreateRasterizerState','CreateSamplerState','CreateQuery',
     'CreatePredicate','CreateCounter','CreateDeferredContext','OpenSharedResource','CheckFormatSupport',
     'CheckMultisampleQualityLevels','CheckCounterInfo','CheckCounter','CheckFeatureSupport','GetFeatureLevel',
     'GetCreationFlags','GetDeviceRemovedReason','GetImmediateContext','SetExceptionMode','GetExceptionMode']},
 'ID3D11DeviceContext': {m:'ctx_'+m for m in ['VSSetConstantBuffers','PSSetShaderResources','PSSetShader','PSSetSamplers',
     'VSSetShader','DrawIndexed','Draw','Map','Unmap','PSSetConstantBuffers','IASetInputLayout','IASetVertexBuffers',
     'IASetIndexBuffer','DrawIndexedInstanced','DrawInstanced','GSSetConstantBuffers','GSSetShader','IASetPrimitiveTopology',
     'VSSetShaderResources','VSSetSamplers','Begin','End','GetData','SetPredication','GSSetShaderResources','GSSetSamplers',
     'OMSetRenderTargets','OMSetRenderTargetsAndUnorderedAccessViews','OMSetBlendState','OMSetDepthStencilState','SOSetTargets',
     'DrawAuto','DrawIndexedInstancedIndirect','DrawInstancedIndirect','Dispatch','DispatchIndirect','RSSetState','RSSetViewports',
     'RSSetScissorRects','CopySubresourceRegion','CopyResource','UpdateSubresource','CopyStructureCount','ClearRenderTargetView',
     'ClearUnorderedAccessViewUint','ClearUnorderedAccessViewFloat','ClearDepthStencilView','GenerateMips','SetResourceMinLOD',
     'GetResourceMinLOD','ResolveSubresource','ExecuteCommandList','HSSetShaderResources','HSSetShader','HSSetSamplers',
     'HSSetConstantBuffers','DSSetShaderResources','DSSetShader','DSSetSamplers','DSSetConstantBuffers','CSSetShaderResources',
     'CSSetUnorderedAccessViews','CSSetShader','CSSetSamplers','CSSetConstantBuffers','ClearState','Flush','GetType',
     'GetContextFlags','FinishCommandList']},
}
MAP['ID3D11Device'].update({'QueryInterface':'m_QueryInterface','AddRef':'m_AddRef','Release':'m_Release',
  'GetPrivateData':'m_GetPrivateData','SetPrivateData':'m_SetPrivateData','SetPrivateDataInterface':'m_SetPrivateDataInterface'})
MAP['ID3D11DeviceContext'].update({'QueryInterface':'m_QueryInterface','AddRef':'m_AddRef','Release':'m_Release','GetDevice':'m_GetDevice',
  'GetPrivateData':'m_GetPrivateData','SetPrivateData':'m_SetPrivateData','SetPrivateDataInterface':'m_SetPrivateDataInterface'})

DXGI = {
 'IDXGIFactory1': dict(DXGIOBJ, EnumAdapters='fac_EnumAdapters',EnumAdapters1='fac_EnumAdapters1',
   MakeWindowAssociation='fac_MakeWindowAssociation',GetWindowAssociation='fac_GetWindowAssociation',
   CreateSwapChain='fac_CreateSwapChain',CreateSoftwareAdapter='fac_CreateSoftwareAdapter',IsCurrent='fac_IsCurrent'),
 'IDXGIAdapter1': dict(DXGIOBJ, EnumOutputs='adp_EnumOutputs',GetDesc='adp_GetDesc',GetDesc1='adp_GetDesc1',
   CheckInterfaceSupport='adp_CheckInterfaceSupport',GetParent='adp_GetParent'),
 'IDXGIOutput': dict(DXGIOBJ, GetDesc='out_GetDesc',GetDisplayModeList='out_GetDisplayModeList',
   FindClosestMatchingMode='out_FindClosestMatchingMode',WaitForVBlank='out_ok1',TakeOwnership='out_ok2',
   ReleaseOwnership='out_ok1',GetGammaControlCapabilities='out_fail2',SetGammaControl='out_fail2',GetGammaControl='out_fail2',
   SetDisplaySurface='out_fail2',GetDisplaySurfaceData='out_fail2',GetFrameStatistics='out_fail2'),
 'IDXGIDevice': dict(DXGIOBJ, GetParent='dxgidev_GetParent',GetAdapter='dxgidev_GetAdapter'),
 'IDXGISwapChain': dict(DXGIOBJ, GetParent='fac_GetParent',GetDevice='sc_GetDevice',Present='sc_Present',GetBuffer='sc_GetBuffer',
   SetFullscreenState='sc_SetFullscreenState',GetFullscreenState='sc_GetFullscreenState',GetDesc='sc_GetDesc',
   ResizeBuffers='sc_ResizeBuffers',ResizeTarget='sc_ResizeTarget',GetContainingOutput='sc_GetContainingOutput',
   GetFrameStatistics='sc_GetFrameStatistics',GetLastPresentCount='sc_GetLastPresentCount'),
}

CLASSMAP = {'ID3D11Device':'C_DEVICE','ID3D11DeviceContext':'C_CONTEXT','ID3D11Buffer':'C_BUFFER','ID3D11Texture1D':'C_TEX1D',
 'ID3D11Texture2D':'C_TEX2D','ID3D11Texture3D':'C_TEX3D','ID3D11RenderTargetView':'C_RTV','ID3D11DepthStencilView':'C_DSV',
 'ID3D11ShaderResourceView':'C_SRV','ID3D11UnorderedAccessView':'C_UAV','ID3D11InputLayout':'C_INPUTLAYOUT',
 'ID3D11VertexShader':'C_VS','ID3D11PixelShader':'C_PS','ID3D11GeometryShader':'C_GS','ID3D11HullShader':'C_HS',
 'ID3D11DomainShader':'C_DS','ID3D11ComputeShader':'C_CS','ID3D11SamplerState':'C_SAMPLER','ID3D11BlendState':'C_BLEND',
 'ID3D11DepthStencilState':'C_DEPTHSTENCIL','ID3D11RasterizerState':'C_RASTERIZER','ID3D11Query':'C_QUERY',
 'IDXGIFactory1':'C_DXGIFACTORY','IDXGIAdapter1':'C_DXGIADAPTER','IDXGIOutput':'C_DXGIOUTPUT','IDXGIDevice':'C_DXGIDEVICE',
 'IDXGISwapChain':'C_DXGISWAPCHAIN'}

stubs=[]
def emit(ifaces, mapping, fn):
    lines=[]
    builds=[]
    for iface in ifaces:
        methods = vtbl(iface)
        mp = mapping[iface]
        arr=[]
        for name,slots in methods:
            h = mp.get(name)
            if not h:
                # a named stub that says once which method the game wanted
                h = f'ni_{iface}_{name}'; stubs.append((h, iface, name, slots))
            arr.append(h)
        var = iface.replace('ID3D11','d3d11_').replace('IDXGI','dxgi_')+'_m'
        lines.append(f'static tm_shim_fn {var}[] = {{ {", ".join(arr)} }};')
        builds.append((CLASSMAP[iface], var, len(arr), iface))
    return lines, builds

d3d_ifaces=['ID3D11Device','ID3D11DeviceContext','ID3D11Buffer','ID3D11Texture1D','ID3D11Texture2D','ID3D11Texture3D',
 'ID3D11RenderTargetView','ID3D11DepthStencilView','ID3D11ShaderResourceView','ID3D11UnorderedAccessView','ID3D11InputLayout',
 'ID3D11VertexShader','ID3D11PixelShader','ID3D11GeometryShader','ID3D11HullShader','ID3D11DomainShader','ID3D11ComputeShader',
 'ID3D11SamplerState','ID3D11BlendState','ID3D11DepthStencilState','ID3D11RasterizerState','ID3D11Query']
dxgi_ifaces=['IDXGIFactory1','IDXGIAdapter1','IDXGIOutput','IDXGIDevice','IDXGISwapChain']
l1,b1 = emit(d3d_ifaces, MAP, None)
l2,b2 = emit(dxgi_ifaces, DXGI, None)

def stub_defs(sel):
    return '\n'.join(f'static void {h}(void) {{ static int w; if (!w) {{ w = 1; TM_WARN("D3D11 bridge: unimplemented {i}::{n} called from guest %08X", g_cur_func); }} RET(0, {s}); }}'
                     for h, i, n, s in stubs if sel(i))
with open('gfx/d3d11_bridge_tables.inc','w') as f:
    f.write('// AUTO-GENERATED by gen_bridge_tables.py\n')
    f.write(stub_defs(lambda i: i.startswith('ID3D11'))+'\n\n')
    f.write('\n'.join(l1)+'\n')
with open('gfx/d3d11_bridge_dxgi_tables.inc','w') as f:
    f.write('// AUTO-GENERATED by gen_bridge_tables.py\n')
    f.write(stub_defs(lambda i: i.startswith('IDXGI'))+'\n\n')
    f.write('\n'.join(l2)+'\n')
    f.write('\nstatic void build_classes(void) {\n')
    for cid,var,n,iface in b1+b2:
        f.write(f'  g_cls[{cid}] = (tm_com_class){{ "{iface}", {n}, {var}, 0, 0, 0, 0 }};\n')
    f.write('}\n')
print('unimplemented method stubs:', len(stubs))
print('generated tables')
