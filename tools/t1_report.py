import collections,sys
d=collections.defaultdict(dict)
for l in open('.scratch/t1/t1.txt'):
    c,s,m,k,v=l.split(); d[(c,s,m)][k]=int(v)
def size(c,s,m,norms=False):
    x=d[(c,s,m)]; return x['terms']+x['term_index']+(x['norms'] if norms else 0)
targets={'bank':(0.875,0.879),'sparsekw':(1.110,1.072),'sparsenum':(0.936,0.881),'wiki':(0.921,None)}
for c in ['bank','sparsekw','sparsenum','wiki']:
    f1,p1=size(c,'all','flat'),size(c,'all','fields')
    print(f"{c:10} one segment: flat {f1:>10} fields {p1:>10} ratio {p1/f1:.3f}  (design {targets[c][0]})  terms/term_index fields {d[(c,'all','fields')]['terms']}/{d[(c,'all','fields')]['term_index']} flat {d[(c,'all','flat')]['terms']}/{d[(c,'all','flat')]['term_index']}")
    if c!='wiki':
        ff=sum(size(c,str(i),'flat') for i in range(5)); pp=sum(size(c,str(i),'fields') for i in range(5))
        print(f"{'':10} five shards:  flat {ff:>10} fields {pp:>10} ratio {pp/ff:.3f}  (design {targets[c][1]})")
