# Sponza

Source: https://github.com/KhronosGroup/glTF-Sample-Models/tree/d7a3cc8e51d7c573771ae77a57f16b0662a905c6/2.0/Sponza
(`glTF/`; the upstream README, quoted below, is `README.upstream.md`). The files are
unmodified.

This model is not under a Creative Commons license. Khronos publishes it with the
licensing notes below, taken from `copyright.txt` in Alexandre Pestana's
`SponzaPBR_Textures.rar`. The grant is Crytek's: the model "is donated to the public for
radiosity and is represented in several different formats (3ds, Obj) for use with various
commercial 3D applications and renderers." It names no terms for modification,
redistribution or credit. OHAO carries the model unmodified, for rendering validation, with
the full credit chain:

- Original Sponza model: Marko Dabrovic, early 2002.
- The Atrium Sponza Palace, Dubrovnik: Frank Meinl, Crytek, August 19, 2010
  (http://www.crytek.com/cryengine/cryengine3/downloads).
- July 14, 2011: Morgan McGuire's corrections to Crytek's OBJ export (masks, bump maps,
  the banner removed).
- PBR textures: Alexandre Pestana (http://www.alexandre-pestana.com/pbr-textures-sponza/),
  repacked into the glTF metallic-roughness layout by the sample model's author
  (MikkTSpace tangents, a constant diffuse factor of about 0.58 kept).

Licensing notes, verbatim from the upstream README:

```
PBR textures for the Sponza model.
For more informations: www.alexandre-pestana.com


Original copyright:

July 14, 2011 Morgan McGuire modified the model from Crytek's OBJ
export to correct some small errors.  He computed bump maps from the
normal maps using <a
href="http://cs.williams.edu/~morgan/code/">normal2bump.cpp</a> (since
MTL files expect height bumps, not normals), put the "mask" textures
into the alpha channel of the associated diffuse texture, cleaned up
noise in the masks, created the missing gi_flag.tga texture, and
removed the long untextured banner floating in the middle of the
atrium that appears in the file but in none of the published images of
the model.  The banner is in banner.obj.



http://www.crytek.com/cryengine/cryengine3/downloads


Sponza Model
August 19, 2010
The Atrium Sponza Palace, Dubrovnik, is an elegant and improved model created by Frank Meinl. The original Sponza model was created by Marko Dabrovic in early 2002. Over the years, the Sponza Atrium scene has become one of the most popular 3D scenes for testing global illumination and radiosity due to it's specific architectural structure which is particularly complex for global illumination light.

However, nowadays it is considered as a simple model, thus it was decided to crate a new model with highly improved appearance and scene complexity. It is donated to the public for  radiosity and is represented in several different formats (3ds, Obj) for use with various commercial 3D applications and renderers.


Screenshot from the I3D paper
http://crytek.com/sites/default/files/20100301_lpv.pdf
```

Included at Frank's instruction (2026-10-25), after the staging notes recorded that this
grant is not a standard license; review it before any public release of the repository.
