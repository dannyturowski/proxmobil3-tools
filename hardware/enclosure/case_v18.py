"""Build the v18 PM3 assembly from the retained v17 Fusion STEP, in mm."""
from pathlib import Path
import itertools
import math
import sys
import cadquery as cq

HERE = Path(__file__).resolve().parent
BASE = HERE / 'source'
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE
OUT.mkdir(exist_ok=True)
V = cq.Vector

def box(x0,x1,y0,y1,z0,z1):
    return cq.Solid.makeBox(x1-x0,y1-y0,z1-z0,V(x0,y0,z0))

def roundbox(x0,x1,y0,y1,z0,z1,r,axis='Z'):
    return cq.Workplane(obj=box(x0,x1,y0,y1,z0,z1)).edges('|'+axis).fillet(r).val()

def cyl(a,b,r):
    direction=V(*b)-V(*a)
    return cq.Solid.makeCylinder(r,direction.Length,V(*a),direction.normalized())

def largest(shape):
    return max(shape.Solids(),key=lambda s:s.Volume())

parts={}
original=largest(cq.importers.importStep(str(BASE/'enclosure-v17.step')).val())
cover=original
# Add 5 mm immediately below the rear panel. Extruding its section preserves
# every through passage, the rear insert bores and their continuous support.
split=51.9
bottom=cover.intersect(box(-110,110,-75,75,-4,split))
top=cover.intersect(box(-110,110,-75,75,split,63))
section=[f for f in top.Faces() if abs(f.Center().z-split)<.00001 and f.BoundingBox().zlen<.00001]
extensions=[cq.Solid.extrudeLinear(f.outerWire(),f.innerWires(),V(0,0,5)) for f in section]
cover=bottom.fuse(*extensions).fuse(top.translate((0,0,5)))
# Carry the exterior R20 silhouette through the thin mating land.
upper=roundbox(-105,105,-70,70,.4,62,20)
cover=cover.intersect(upper.fuse(roundbox(-105,105,-70,70,-4,.4,20)))
# Replace the complete legacy corner blocks, including pierced outer walls.
# A constant-thickness rounded rim surrounds each full circular post.
rim=upper.cut(roundbox(-102.6,102.6,-67.6,67.6,.39,58.6,17.6))
for x,dy in ((-93.99,53.795),(91.39,56.98)):
    for sy in (-1,1):
        y=sy*dy
        x0,x1=(-106,-80) if x<0 else (80,106)
        y0,y1=(-71,-45) if sy<0 else (45,71)
        zone=box(x0,x1,y0,y1,.4,61)
        # The two inner-facing tangents meet the adjacent perimeter walls.
        # Only the inner quarter of the round post is exposed to the cavity.
        sx=1 if x>0 else -1
        radius=5.5
        side=box(*sorted((x-sx*radius,sx*108)),*sorted((y,sy*73)),1.5,61)
        end=box(*sorted((x,sx*108)),*sorted((y-sy*radius,sy*73)),1.5,61)
        column=cyl((x,y,0),(x,y,61),radius).fuse(side,end).clean()
        slab=box(x-30,x+30,y-30,y+30,58.6,61)
        blend=column.fuse(slab).clean()
        roots=[e for e in blend.Edges() if abs(e.Center().z-58.6)<.001
               and e.BoundingBox().zlen<.001 and e.Length()<55]
        blend=blend.fillet(1.2,roots)
        corner=rim.fuse(blend).intersect(zone).intersect(upper)
        cover=cover.cut(zone).fuse(corner)
        # Rebuild the PM3-facing lip all the way down to its free edge.
        # Concentric arcs keep the 2.6 mm lip and 2.4 mm wall continuous.
        lip_zone=box(x0,x1,y0,y1,-2.71,.4)
        lip=roundbox(-105,105,-70,70,-2.71,.4,20)
        lip=lip.cut(roundbox(-102.4,102.4,-67.4,67.4,-2.72,.2,17.4),
                    roundbox(-102.6,102.6,-67.6,67.6,.2,.41,17.6))
        cover=cover.cut(lip_zone).fuse(lip.intersect(lip_zone))
        cover=cover.cut(cyl((x,y,-1),(x,y,61.1),2.6),cyl((x,y,12),(x,y,61.1),4.25))
        # Print from +Z: two short bridge courses form the flat screw seat
        # across the counterbore. Both are reliefs, outside the head envelope.
        cb=cyl((x,y,11.59),(x,y,12.01),4.25)
        for a,b,span in ((11.8,12.001,5),(11.6,11.8,2.6)):
            cover=cover.cut(cb.intersect(box(x-span,x+span,y-2.6,y+2.6,a,b)))
        # The diagonal strap bores crossed the flat walls beyond the old
        # blocks. Restore the wall across both ends of each former tunnel.
        u,v=sorted((72 if x>0 else -106,106 if x>0 else -72))
        m,n=sorted((sy*35,sy*71))
        cover=cover.fuse(rim.intersect(box(u,v,m,n,13,19)))
        # Clear the short diagonal remnants inboard of the rebuilt walls.
        interior=roundbox(-102.6,102.6,-67.6,67.6,13,19,17.6)
        cover=cover.cut(interior.intersect(box(u,v,m,n,13,19)).cut(column))
# Clear the three obsolete insert pads and their joining ribs across the
# scanner end. Restore a continuous rear panel through their bores.
cover=cover.cut(box(78,102.6,-44.99,44.99,35,58.6))
cover=cover.fuse(box(79,101,-42,42,58.6,61))
# Clear the old battery locator ribs before placing stops for the moved pack.
cover=cover.cut(box(79.45,81.55,-67.6,20.66,43.55,58.6),
                box(-66.15,81.55,18.59,20.66,43.55,58.6))
# Restore the continuous +Y sidewall at the original R11 carrier screws
# and plunger holes. All four were at Z44, X=-57 +/-9 and +/-18.
cover=cover.fuse(box(-78.5,-35.5,67.6,70,40.5,47.5)).clean()
# Replace integral finger tunnels. Both sleeves insert from outside, index
# in their rounded rectangular sockets and stop against an exterior flange.
def tunnel(name,axis,start,end,c,h,w,tall,r,old_w,old_h,old_r,screw_side,shift=0):
    global cover
    def profile(lo,hi,width,height,radius):
        a,b=c-width/2,c+width/2
        args=(lo,hi,a,b) if axis=='X' else (a,b,lo,hi)
        return roundbox(*args,h-height/2,h+height/2,radius,axis)
    mouth=start if axis=='X' else end
    sign=-1 if axis=='X' else 1
    # Only excise the old sleeve inside the wall, then cut the new socket.
    lo,hi=(start+2.4,end+.05) if axis=='X' else (start-.05,end-2.4)
    cover=cover.cut(profile(lo,hi,old_w+.1,old_h+.1,old_r+.05))
    if shift:
        # Fill the previous rocker mouth only through the case wall before
        # cutting the opening aligned with the new pack position.
        cover=cover.fuse(profile(start,start+2.4,old_w,old_h,old_r))
        h+=shift
    cover=cover.cut(profile(start-.1,end+.1,w+3.4,tall+3.4,r+1.7))
    shell=profile(start,end,w+2.8,tall+2.8,r+1.4)
    flo,fhi=sorted((mouth-sign*1.8,mouth))
    p=c+screw_side*(w/2+5.3)
    def screw_line(lo,hi,radius):
        return cyl((lo,p,h),(hi,p,h),radius) if axis=='X' else cyl((p,lo,h),(p,hi,h),radius)
    blo,bhi=sorted((mouth,mouth-sign*6.5))
    # Back panel prints down. This haunch grows out of the wall by less
    # than one mm per mm of build height and supports the side-facing bore.
    wall=mouth-sign*2.0
    tip=mouth-sign*6.5
    profile_points=[(wall,h),(tip,h),(tip,h+3.6),(wall,h+9.2)]
    if axis=='X':
        haunch=cq.Workplane('XZ').polyline(profile_points).close().extrude(7.2).val().translate((0,p+3.6,0))
    else:
        haunch=cq.Workplane('YZ').polyline(profile_points).close().extrude(7.2).val().translate((p-3.6,0,0))
    support=screw_line(blo,bhi,3.6).fuse(haunch)
    inner=mouth-sign*2.4
    face_a,face_b=sorted((mouth,inner))
    slab=(box(face_a,face_b,p-7,p+7,h-7,h+12) if axis=='X' else
          box(p-7,p+7,face_a,face_b,h-7,h+12))
    support=support.fuse(slab).clean()
    roots=[]
    for e in support.Edges():
        q=e.Center()
        axial,lateral=(q.x,q.y) if axis=='X' else (q.y,q.x)
        if abs(axial-inner)<.001 and abs(lateral-p)<3.65 and h-3.61<q.z<h+9.2:
            roots.append(e)
    support=support.fillet(.65,roots).cut(slab)
    cover=cover.fuse(support).cut(profile(start-.1,end+.1,w+3.4,tall+3.4,r+1.7))
    cover=cover.cut(screw_line(blo-.01,bhi+.01,1.25))
    flange=profile(flo,fhi,w+6,tall+6,r+3).fuse(screw_line(flo,fhi,4)).clean()
    shoulder=c+screw_side*(w/2+3)
    roots=[e for e in flange.Edges() if abs(e.Length()-1.8)<.001 and
           abs((e.Center().y if axis=='X' else e.Center().x)-shoulder)<.001
           and abs(e.Center().z-h)<4]
    assert len(roots)==2,(name,len(roots))
    flange=flange.fillet(2.5,roots)
    face=max(flange.Faces(),key=lambda f:sign*(f.Center().x if axis=='X' else f.Center().y))
    outline=face.outerWire().offset2D(.22)[0]
    vector=V(-sign*1.8,0,0) if axis=='X' else V(0,-sign*1.8,0)
    cover=cover.cut(cq.Solid.extrudeLinear(outline,[],vector))
    shell=shell.fuse(flange).cut(screw_line(flo-.01,fhi+.01,1.65))
    entry=V(mouth,p,h) if axis=='X' else V(p,mouth,h)
    shell=shell.cut(cq.Solid.makeCone(3.1,1.65,1.45,entry,vector.normalized()))
    shell=shell.cut(profile(min(start,flo)-1,max(end,fhi)+1,w,tall,r))
    if axis=='Y':
        shell=shell.cut(box(-36.8,-18.8,17,31.5,0,8))
    parts[name]=shell.clean()

pack_shift=5.6
pack_down=5.1
# The linked rocker paddle center is Z40 before moving the pack. The
# inherited tunnel was 4 mm low; center this opening on the actual paddle.
tunnel('Battery rocker tunnel','X',-105,-71.5+pack_down,-22.5,36,30,20,3,35,25,5.5,-1,pack_shift+4)
tunnel('PROXdongle tunnel','Y',18,70,-27.8,20,30,26,4,34.8,30.8,6.4,-1)
# Both generations of mounting pads fall within this wall patch.
cover=cover.cut(box(98.9,105,21.9,48.1,2.9,12.1)).fuse(box(102.6,105,21.9,48.1,2.9,12.1))
# One cantilever secured entirely on the +Y side of the battery. Its two
# screw seats carry preload; the tongue overlaps the pack by 34 mm.
mount_x=(0,15)
pad=roundbox(-4,19,19.8,28,30.3,59,3)
local_floor=box(-8,23,16,32,58.6,61)
pad=pad.fuse(local_floor).clean()
roots=[e for e in pad.Edges() if abs(e.Center().z-58.6)<.001 and e.BoundingBox().zlen<.001
       and -4.1<e.Center().x<19.1 and 19.7<e.Center().y<28.1]
pad=pad.fillet(1.2,roots).intersect(box(-8,23,16,32,30,58.6))
cover=cover.fuse(pad)
bridge=roundbox(-4,19,-16,28,25.9,30.3,3)
for x in mount_x:
    cover=cover.cut(cyl((x,24,30.2),(x,24,39),1.25))
    bridge=bridge.cut(cyl((x,24,25.8),(x,24,30.4),1.65))
# Leave the USB end open. Set the far end and the side faces independently.
for x in (-51,57):
    cover=cover.fuse(box(x-8,x+8,18.6,20.6,48.6,59))
# Two short stops on the connector end are outside both plug paths.
near_end=-66.1+pack_down-.6
for y in (-63,15):
    cover=cover.fuse(box(near_end-2,near_end,y-2,y+2,49,59))
# Retention is by bridge now. Close both strap routes, including rear exits.
for x in (-45,22):
    cover=cover.fuse(box(x-11.2,x+11.2,-70,-67.6,20.35,25.65),
                     box(x-11.2,x+11.2,18.9,22.1,58.6,61))
# The accelerometer pilots were blind. Restore deck material only.
for x in (42.85,65.15):
    cover=cover.cut(cyl((x,30,45),(x,30,58.6),2.76))
    cover=cover.fuse(cyl((x,30,58.6),(x,30,60.95),1.12))
# Revolve the concave R1.2 junction into each existing suction insert pad.
root_radius=1.2
root=(cq.Workplane('XZ').moveTo(8.9,58.6).lineTo(8.9,57.4)
      .lineTo(9,57.4).threePointArc((10.2-root_radius/math.sqrt(2),57.4+root_radius/math.sqrt(2)),(10.2,58.6))
      .close().revolve(360,(0,0),(0,1)).val())
for x in (-40,25):
    cover=cover.fuse(root.translate((x,32,0)))
# Mirrored grilles below battery level and outside the side module.
for side in (-1,1):
    drills=[]
    for row,count in enumerate((3,4,5,4,3)):
        z=8+3.5*row
        for col in range(count):
            x=61+4*(col-(count-1)/2)
            drills.append(cyl((x,side*65,z),(x,side*71,z),1.4))
    cover=cover.cut(*drills)
# Bottom end (+X); offset toward +Y to keep the central scanner path clear.
for row,count in enumerate((3,4,5,4,3)):
    for col in range(count):
        y=32+4*(col-(count-1)/2)
        z=29+3.5*row
        cover=cover.cut(cyl((101,y,z),(106,y,z),1.4))
parts['Battery retainer']=bridge
# Right side is -Y, front is -Z, screen vertical is X. Sponson drops
# forward to bring the sensor to the front plane. Service through rear lid.
outer=box(-41,29,-100,-70,-44,23)
long_edges=[e for e in outer.Edges() if abs(e.Center().y+100)<.001 and e.BoundingBox().zlen>66]
assert len(long_edges)==2
outer=outer.fillet(8,long_edges)
rims=[e for e in outer.Edges() if e.BoundingBox().zlen<.001]
outer=outer.fillet(.8,rims)
pod=outer
cavity=box(-38.6,26.6,-97.6,-72.4,-41.6,26)
cavity=cavity.fillet(5.6,[e for e in cavity.Edges() if abs(e.Center().y+97.6)<.001 and e.BoundingBox().zlen>66])
pod=pod.cut(cavity)
# A split at the last 3 mm leaves the lid flush with the rounded body.
lid=outer.intersect(box(-45,45,-112,-66,20,25))
pod=pod.cut(box(-45,45,-112,-66,20,25))

# Front controls share the same lateral center. -X is up on the reader.
sensor_x=9
toggle_x=-23
toggle_y=-85
pod=pod.cut(cyl((toggle_x,toggle_y,-45),(toggle_x,toggle_y,-40),6.15))
pod=pod.fuse(box(toggle_x+5.3,toggle_x+6.65,toggle_y-.9,toggle_y+.9,-44,-41.6))
# Install each tact sideways from the interior into a circular side bore.
# The front saddle carries the press; two lid fingers capture each body.
def rib(x0,x1,points):
    return cq.Workplane('YZ').polyline(points).close().extrude(x1-x0).val().translate((x0,0,0))
tact_z=(-44+23)/2
# One continuous web holds both switches. Short toes leave wire channels.
lid=lid.fuse(box(-17,5,-93.15,-89.6,tact_z+5,20.2))
for x in (-16.5,3.5):
    lid=lid.fuse(rib(x,x+1.5,[(-90,1),(-84,20.2),(-90,20.2)]))
for x in (-14,2):
    pod=pod.fuse(box(x-5,x+5,-98,-90.8,tact_z-5,tact_z+4.5),
                 rib(x-5,x+5,[(-98,tact_z-13),(-90.8,tact_z-5),(-98,tact_z-4.9)]))
    pod=pod.cut(box(x-3.25,x+3.25,-97.65,-89,tact_z-3.25,tact_z+5))
    pod=pod.cut(cyl((x,-101,tact_z),(x,-96.5,tact_z),2.05))
    pod=pod.cut(cyl((x,-101,tact_z),(x,-99.15,tact_z),4.8))
    # Fork leaves a channel for wires between the two retaining fingers.
    for dx in (-1.75,1.75):
        lid=lid.fuse(box(x+dx-.8,x+dx+.8,-93.15,-90.95,tact_z-2,tact_z+5.2))
    lid=lid.fuse(rib(x-2.85,x+2.85,[(-97.35,tact_z+3.25),(-91.1,tact_z+3.25),
                    (-91.1,tact_z+10),(-93.15,tact_z+10),(-97.35,tact_z+4.8)]))
sensor_y=toggle_y+1.3
window_y=sensor_y-1.3
def window(w,h,z0,z1):
    return cq.Workplane('XY').workplane(offset=z0).center(sensor_x,window_y).slot2D(w,h).extrude(z1-z0).val()
aperture=cq.Workplane('XY').workplane(offset=-44.01).center(sensor_x,window_y).slot2D(11.5,6.9)
lead=aperture.workplane(offset=.76).slot2D(10.5,5.9).loft().val()
pod=pod.cut(window(10.5,5.9,-45,-41.5)).cut(lead)
for x in (sensor_x-9.875,sensor_x+9.875):
    pod=pod.fuse(cyl((x,sensor_y,-42),(x,sensor_y,-37.6),2.45))
    pod=pod.cut(cyl((x,sensor_y,-43),(x,sensor_y,-37.5),.85))
# Relieve only the rearward insertion corridor through the upper tact saddle.
# Its front seat and side actuator bore still locate the switch after assembly.
insertion=box(toggle_x-15.1,toggle_x+15.1,toggle_y-8.25,toggle_y+8.25,-41.59,24)
pod=pod.cut(insertion)
# Two M3 case screws from inside the enclosure; wire exit between.
for x in (-31,20):
    pod=pod.fuse(cyl((x,-77,8),(x,-72,8),3.8),
                 rib(x-3.8,x+3.8,[(-72,0),(-77,5.5),(-77,8),(-72,8)]))
    pod=pod.cut(cyl((x,-76.6,8),(x,-69,8),1.25))
    cover=cover.cut(cyl((x,-71,8),(x,-65,8),1.65))
wire=roundbox(-11,11,-74,-64,2,17,4,'Y')
def cable_passage(s,faces):
    s=s.cut(wire)
    for y in faces:
        edges=[]
        for e in s.Edges():
            b=e.BoundingBox()
            if (abs(b.ymin-y)<.001 and abs(b.ymax-y)<.001 and
                b.xmin>=-11.001 and b.xmax<=11.001 and b.zmin>=1.999 and b.zmax<=17.001):
                edges.append(e)
        assert len(edges)==8,(y,len(edges))
        s=s.fillet(.7,edges)
    return s
pod=cable_passage(pod,(-70,-72.4))
cover=cable_passage(cover,(-70,-67.6))
for x in (-33,21):
    y=-74.5
    pod=pod.fuse(cyl((x,y,12),(x,y,20),2.8),box(x-2,x+2,y,-72,12,20),
                 rib(x-2.8,x+2.8,[(-72,6),(-77.3,12),(-77.3,12.2),(-72,12.2)]))
    pod=pod.cut(cyl((x,y,12.5),(x,y,20.5),1.05))
    lid=lid.cut(cyl((x,y,19),(x,y,24),1.4))
# Remove the two legacy upper pads, their short ribs and through holes.
# The M6 suction pads farther down the deck have independent axes.
for sy in (-1,1):
    y=sy*30
    cover=cover.cut(box(-97.05,-76.95,*sorted((sy*19.95,sy*45)),47.59,58.6))
    cover=cover.fuse(cyl((-87,y,58.6),(-87,y,61),3.35))
for sx,sy in itertools.product((-1,1), repeat=2):
    for angle in (0,15,30,45,60,75,90):
        rad=math.radians(angle)
        for z,inner in ((-2.65,17.4),(-1,17.4),(.3,17.6)):
            for radius,expected in ((inner-.05,False),(inner+.05,True),(19.95,True),(20.05,False)):
                point=V(sx*(85+radius*math.cos(rad)),sy*(50+radius*math.sin(rad)),z)
                assert cover.isInside(point)==expected,(sx,sy,angle,z,radius)
parts.update({'Right sponson':pod.clean(),'Sponson service lid':lid.clean(),'Back cover':cover})
def validate(name,s):
    assert len(s.Solids())==1 and s.isValid(), (name,len(s.Solids()))
for name,s in parts.items():
    validate(name,s)
for (a,s),(b,t) in itertools.combinations(parts.items(),2):
    overlap=s.intersect(t).Volume()
    assert overlap<.01,(a,b,overlap)
battery=box(-66.1+pack_down,78.9+pack_down,-67,18,25+pack_shift,53+pack_shift)
assert all(s.intersect(battery).Volume()<.01 for s in parts.values())
toggle_insertion=box(toggle_x-15,toggle_x+15,toggle_y-7.5,toggle_y+7.5,-41.59,24)
assert pod.intersect(toggle_insertion).Volume()<.01
# Conservatively fill the whole body footprint behind the 18 mm case for
# the catalogued 9.2 mm screw-terminal depth, with the lid and tacts fitted.
hardware=box(toggle_x-15,toggle_x+15,toggle_y-7.5,toggle_y+7.5,-41.6,-41.6+18+9.2)
assert all(s.intersect(hardware).Volume()<.01 for s in (pod,lid))
for x in (sensor_x-9.875,sensor_x+9.875):
    driver=cyl((x,sensor_y,-36.59),(x,sensor_y,24),2)
    assert pod.intersect(driver).Volume()<.01
for x in (-14,2):
    tact=box(x-3,x+3,-97.55,-93.35,tact_z-3,tact_z+3)
    assert sum(s.intersect(tact).Volume() for s in (pod,lid))<.01
for x in (-31,20):
    tool=cyl((x,-67.59,8),(x,-42,8),3)
    assert cover.intersect(tool).Volume()<.01
for x in mount_x:
    tool=cyl((x,24,-1),(x,24,25.89),3)
    assert cover.intersect(tool).Volume()<.01
# Usable straight approach across the modeled USB plug width and height.
usb=box(-88,-66.1+pack_down,-57.8,-49.3,31.5+pack_shift,48.5+pack_shift)
assert all(s.intersect(usb).Volume()<.01 for s in parts.values())
assembly=cq.Assembly(name='PM3 enclosure v18')
for name,s in parts.items():
    assembly.add(s,name=name)
assembly.export(str(OUT/'enclosure-v18.step'))
loaded=cq.importers.importStep(str(OUT/'enclosure-v18.step')).solids().vals()
assert len(loaded)==len(parts) and all(s.isValid() for s in loaded)
print('Verified',len(loaded),'solids; all printed-part pairs clear.')
