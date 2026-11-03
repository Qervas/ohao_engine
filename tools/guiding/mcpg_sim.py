"""Markov chain path guiding at one vertex, on the CPU (sub-project 23; spec "Learning").

One floor point x = (0, 0, 0), normal +y, under a bright disc on the ceiling (radius 0.3 at (2.5, 3, -2.5), radiance P)
and dim walls (radiance Wd elsewhere). 64 samples a frame resample 5 of 64 states, draw from the BSDF/vMF mixture as
shaders/rt/path_tracer.slang does, and learn by Algorithm 3 of Alber, Hanika, Dachsbacher (2025), from the paper's text.
It compares the write rules: the paper's (a fresh slot, the last writer wins) and the engine's (the occupant stays
unless a coin u * w_occupant < w_new says so), by how many runs end with states pointing at the patch and by the
estimator's variance.

    python tools/guiding/mcpg_sim.py [--runs 8] [--frames 300]
"""
import argparse
import numpy as np, math
rng=np.random.default_rng(1)   # reseeded per run
P, Wd = 200.0, 0.05
pc=np.array([2.5,3.0,-2.5]); pr=0.3
def radiance(d):
    # ray from origin along d hits the ceiling plane y=3
    if d[1] <= 1e-6: return Wd, d*10
    t=3.0/d[1]; hit=d*t
    if np.linalg.norm(hit-pc) < pr: return P, hit
    return Wd, hit
def cos_sample():
    u1,u2=rng.random(),rng.random(); r=math.sqrt(u1); ph=2*math.pi*u2
    return np.array([r*math.cos(ph), math.sqrt(max(0,1-u1)), r*math.sin(ph)])
def vmf_pdf(d,mu,k):
    if k<1e-4: return 1/(4*math.pi)
    return k/(2*math.pi*(1-math.exp(-2*k)))*math.exp(k*(d@mu-1))
def vmf_sample(mu,k):
    u1,u2=rng.random(),rng.random()
    t=-math.log(u1+(1-u1)*math.exp(-2*k))/k if k>1e-4 else 2*u1
    w=1-t; s=math.sqrt(max(t*(2-t),0)); ph=2*math.pi*u2
    a=np.array([1,0,0]) if abs(mu[0])<0.9 else np.array([0,1,0])
    b1=np.cross(mu,a); b1/=np.linalg.norm(b1); b2=np.cross(mu,b1)
    d=w*mu+s*(math.cos(ph)*b1+math.sin(ph)*b2); return d/np.linalg.norm(d)
def kappa(r):
    r=min(max(r,0),0.9999); k=(3*r-r**3)/(1-r*r)
    if k>1e-3:
        e=math.exp(-2*k); A=(1+e)/(1-e)-1/k; dA=1/k**2-4*e/(1-e)**2
        if dA>0: k=max(k-(A-r)/dA,0.5*k)
    return min(k,1e4)
NSLOT=64; NC=5
def run(frames, share, n0, occupant, train=True):
    S=[None]*NSLOT  # state: dict w,y,r,n
    ests=[]
    for f in range(frames):
        props=[]
        for px in range(64):   # 64 samples (pixels) a frame at this point
            cand=[S[rng.integers(NSLOT)] for _ in range(NC)]
            lobes=[]
            for c in cand:
                if c is None or c['w']<=0: lobes.append(None); continue
                d=c['y']/c['w']; dist2=d@d; mu=d/math.sqrt(dist2); n=c['n']; np_=n0+1/dist2
                rb=(n*n*(c['r']/c['w']))/(n*n+np_); k=kappa(rb)
                lobes.append((mu,k,c['w'],c))
            W=sum(l[2] for l in lobes if l)
            g=share if W>0 else 0
            src=None
            if rng.random()<g:
                ws=np.array([l[2] if l else 0 for l in lobes]); i=rng.choice(NC,p=ws/W); mu,k,_,src=lobes[i]; d=vmf_sample(mu,k)
            else: d=cos_sample()
            pb=max(d[1],0)/math.pi
            q=(1-g)*pb + (g*sum(l[2]*vmf_pdf(d,l[0],l[1]) for l in lobes if l)/W if W>0 else 0)
            L,hit=radiance(d)
            fval = (max(d[1],0)/math.pi*0.8)*L/q if q>0 else 0   # f cos L / q, albedo 0.8
            ests.append(fval)
            props.append((fval,hit,src,W/NC))
        if not train: continue
        newS=list(S)
        for fval,hit,src,meanW in props:
            if fval<=0: continue
            if rng.random()*meanW < fval:
                s=src or {'w':0,'y':np.zeros(3),'r':0,'n':0}
                dirn=hit/np.linalg.norm(hit)
                mu=dirn if s['w']<=0 else (s['y']/s['w'])/np.linalg.norm(s['y']/s['w'])
                n=min(s['n']+1,64); a=max(1/n,0.02)
                o={'w':s['w']+(fval-s['w'])*a,'y':s['y']+(fval*hit-s['y'])*a,'r':s['r']+(fval*(dirn@mu)-s['r'])*a,'n':n}
                slot=rng.integers(NSLOT); old=S[slot]
                if not occupant or old is None or old['w']<=0 or rng.random()*old['w']<o['w']:
                    newS[slot]=o
        S=newS
    e=np.array(ests[-64*50:]); return e.mean(), e.var(), S
def patch_states(S):
    k=0
    for c in S:
        if c and c['w']>0:
            d=c['y']/c['w']; mu=d/np.linalg.norm(d)
            if mu@((pc)/np.linalg.norm(pc))>0.99: k+=1
    return k

if __name__=='__main__':
    ap=argparse.ArgumentParser(); ap.add_argument('--runs',type=int,default=8); ap.add_argument('--frames',type=int,default=300)
    a=ap.parse_args()
    for occupant in (False,True):
        found=0; vs=[]
        for seed in range(a.runs):
            rng=np.random.default_rng(seed)
            m,v,S=run(a.frames,0.5,1.0,occupant)
            k=patch_states(S); found+=k>0; vs.append(v)
            print(f"{'occupant' if occupant else 'paper   '} seed {seed}: mean {m:.4f} var {v:.3f} patch states {k}")
        print(f"{'occupant' if occupant else 'paper'}: {found} of {a.runs} runs learned the patch; variance {min(vs):.2f} to {max(vs):.2f}")
