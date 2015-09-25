/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "SplatTerrainEffect"
#include "SplatOptions"
#include "Biome"

#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/VirtualProgram>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/ImageUtils>
#include <osgEarth/URI>
#include <osgEarth/ShaderLoader>
#include <osgEarthUtil/SimplexNoise>

#include <osg/Texture2D>
#include <osgDB/WriteFile>

#include "SplatShaders"

#define LC "[Splat] "

#define COVERAGE_SAMPLER "oe_splat_coverageTex"
#define SPLAT_SAMPLER    "oe_splatTex"
#define NOISE_SAMPLER    "oe_splat_noiseTex"

using namespace osgEarth;
using namespace osgEarth::Splat;

SplatTerrainEffect::SplatTerrainEffect() :
_renderOrder ( -1.0f ),
_editMode    ( false ),
_gpuNoise    ( false )
{
    _scaleOffsetUniform = new osg::Uniform("oe_splat_scaleOffsetInt", 0 );
    _warpUniform        = new osg::Uniform("oe_splat_warp",           0.0f );
    _blurUniform        = new osg::Uniform("oe_splat_blur",           1.0f );
    _useBilinearUniform = new osg::Uniform("oe_splat_useBilinear",    1.0f );
    _noiseScaleUniform  = new osg::Uniform("oe_splat_noiseScale",    12.0f );

    //_scaleOffsetUniform    = new osg::Uniform("oe_splat_scaleOffsetInt",   *def.scaleLevelOffset());
    //_warpUniform           = new osg::Uniform("oe_splat_warp",             *def.coverageWarp());
    //_blurUniform           = new osg::Uniform("oe_splat_blur",             *def.coverageBlur());
    //_useBilinearUniform    = new osg::Uniform("oe_splat_useBilinear",      (def.bilinearSampling()==true?1.0f:0.0f));
    //_noiseScaleUniform     = new osg::Uniform("oe_splat_noiseScale",       12.0f);

    _editMode = (::getenv("OSGEARTH_SPLAT_EDIT") != 0L);
    _gpuNoise = (::getenv("OSGEARTH_SPLAT_GPU_NOISE") != 0L);
}

void
SplatTerrainEffect::setDBOptions(const osgDB::Options* dbo)
{
    _dbo = dbo;
}

void
SplatTerrainEffect::gogolandcover(TerrainEngineNode* engine)
{
    if ( !_landCover.valid() )
        return;

    if ( engine )
    {
        for(LandCoverLayers::const_iterator i = _landCover->getLayers().begin();
            i != _landCover->getLayers().end();
            ++i)
        {
            const LandCoverLayer* layer = i->get();
            if ( layer )
            {
                OE_INFO << LC << "Adding land cover group: " << layer->getName() << " at LOD " << layer->getLOD() << "\n";

                osg::StateSet* stateset = engine->addLandCoverGroup( layer->getName(), layer->getLOD() );
                if ( stateset )
                {
                    // Install the land cover shaders on the state set
                    VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
                    LandCoverShaders shaders;
                    shaders.loadAll( vp, _dbo.get() );

                    // Install the uniforms
                    stateset->addUniform( new osg::Uniform("oe_landcover_maxDistance", 6400.0f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_windFactor", 0.0f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_noise", 1.5f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_density", 1.0f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_ao", 0.5f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_colorVariation", 1.0f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_exposure", 1.8f) );

                    // TODO: move elsewhere
                    stateset->addUniform( new osg::Uniform("oe_landcover_width", 15.0f) );
                    stateset->addUniform( new osg::Uniform("oe_landcover_height", 20.0f) );

                    int unit;
                    if ( engine->getResources()->reserveTextureImageUnit(unit, "LandCover") )
                    {
                        osg::Image* image = URI("h:/devel/osgearth/repo/tests/flora/pine.png").getImage(_dbo.get());
                        if ( image )
                        {
                            osg::Texture2D* tex = new osg::Texture2D(image);
                            tex->setFilter(tex->MIN_FILTER, tex->NEAREST_MIPMAP_LINEAR);
                            tex->setFilter(tex->MAG_FILTER, tex->LINEAR);
                            tex->setWrap  (tex->WRAP_S, tex->REPEAT);
                            tex->setWrap  (tex->WRAP_T, tex->REPEAT);
                            tex->setUnRefImageDataAfterApply( true );
                            tex->setMaxAnisotropy( 4.0 );
                            tex->setResizeNonPowerOfTwoHint( false );

                            stateset->setTextureAttribute(unit, tex);
                            stateset->addUniform(new osg::Uniform("oe_landcover_tex", unit) );
                        }
                        else
                        {
                            OE_WARN << LC << "Failed to load the tree!!!\n";
                        }
                    }
                }
            }
        }
    }
}

void
SplatTerrainEffect::onInstall(TerrainEngineNode* engine)
{
    if ( engine )
    {
        // Check that we have coverage data (required for now - later masking data will be an option)
        if ( !_coverage.valid() || !_coverage->hasLayer() )
        {
            OE_WARN << LC << "ILLEGAL: coverage data is required\n";
            return;
        }

        if ( !_surface.valid() )
        {
            OE_WARN << LC << "Note: no surface information available\n";
        }

        // First, create a surface splatting texture array for each biome region.
        if ( _coverage.valid() && _surface.valid() )
        {
            if ( createSplattingTextures(_coverage.get(), _surface.get(), _textureDefs) == false )
            {
                OE_WARN << LC << "Failed to create any valid splatting textures\n";
                return;
            }

            // Set up surface splatting:
            if ( engine->getResources()->reserveTextureImageUnit(_splatTexUnit, "Splat Coverage Data") )
            {
                osg::StateSet* stateset;

                if ( _surface->getBiomeRegions().size() == 1 )
                    stateset = engine->getSurfaceStateSet();
                else
                    stateset = new osg::StateSet();

                // TODO: reinstate "biomes"
                //osg::StateSet* stateset = new osg::StateSet();

                // surface splatting texture array:
                _splatTexUniform = stateset->getOrCreateUniform( SPLAT_SAMPLER, osg::Uniform::SAMPLER_2D_ARRAY );
                _splatTexUniform->set( _splatTexUnit );
                stateset->setTextureAttribute( _splatTexUnit, _textureDefs[0]._texture.get() );

                // coverage code sampler:
                osg::ref_ptr<ImageLayer> coverageLayer;
                _coverage->lockLayer( coverageLayer );

                _coverageTexUniform = stateset->getOrCreateUniform( COVERAGE_SAMPLER, osg::Uniform::SAMPLER_2D );
                _coverageTexUniform->set( coverageLayer->shareImageUnit().get() );

                // control uniforms (TODO: simplify and deprecate unneeded uniforms)
                stateset->addUniform( _scaleOffsetUniform.get() );
                stateset->addUniform( _warpUniform.get() );
                stateset->addUniform( _blurUniform.get() );
                stateset->addUniform( _noiseScaleUniform.get() );
                stateset->addUniform( _useBilinearUniform.get() );

                stateset->addUniform(new osg::Uniform("oe_splat_detailRange",  1000000.0f));


                SplattingShaders splatting;

                splatting.define( "SPLAT_EDIT",        _editMode );
                splatting.define( "SPLAT_GPU_NOISE",   _gpuNoise );
                splatting.define( "OE_USE_NORMAL_MAP", engine->normalTexturesRequired() );

                splatting.replace( "$COVERAGE_TEXTURE_MATRIX", coverageLayer->shareTexMatUniformName().get() );
            
                VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
                splatting.load( vp, splatting.VertModel );
                splatting.load( vp, splatting.VertView );
                splatting.load( vp, splatting.Frag );
                splatting.load( vp, splatting.Util );

                // GPU noise is expensive, so only use it to tweak noise function values that you
                // can later bake into the noise texture generator.
                if ( _gpuNoise )
                {                
                    //osgEarth::replaceIn( fragmentShader, "#undef SPLAT_GPU_NOISE", "#define SPLAT_GPU_NOISE" );

                    // Use --uniform on the command line to tweak these values:
                    stateset->addUniform(new osg::Uniform("oe_splat_freq",   32.0f));
                    stateset->addUniform(new osg::Uniform("oe_splat_pers",    0.8f));
                    stateset->addUniform(new osg::Uniform("oe_splat_lac",     2.2f));
                    stateset->addUniform(new osg::Uniform("oe_splat_octaves", 8.0f));
                }
                else // use a noise texture (the default)
                {
                    if (engine->getResources()->reserveTextureImageUnit(_noiseTexUnit, "Splat Noise"))
                    {
                        _noiseTex = createNoiseTexture();
                        stateset->setTextureAttribute( _noiseTexUnit, _noiseTex.get() );
                        _noiseTexUniform = stateset->getOrCreateUniform( NOISE_SAMPLER, osg::Uniform::SAMPLER_2D );
                        _noiseTexUniform->set( _noiseTexUnit );
                    }
                }

                if ( _gpuNoise )
                {
                    // support shaders
                    std::string noiseShaderSource = ShaderLoader::load( splatting.Noise, splatting );
                    osg::Shader* noiseShader = new osg::Shader(osg::Shader::FRAGMENT, noiseShaderSource);
                    vp->setShader( "oe_splat_noiseshaders", noiseShader );
                }

                // TODO: disabled biome selection temporarily because the callback impl applies the splatting shader
                // to the land cover bin as well as the surface bin, which we do not want -- find another way
                if ( _surface->getBiomeRegions().size() == 1 )
                {
                    // install his biome's texture set:
                    stateset->setTextureAttribute(_splatTexUnit, _textureDefs[0]._texture.get());

                    // install this biome's sampling function. Use cloneOrCreate since each
                    // stateset needs a different shader set in its VP.
                    VirtualProgram* vp = VirtualProgram::cloneOrCreate( stateset );
                    osg::Shader* shader = new osg::Shader(osg::Shader::FRAGMENT, _textureDefs[0]._samplingFunction);
                    vp->setShader( "oe_splat_getRenderInfo", shader );
                }

                else
                {
                    OE_WARN << LC << "Multi-biome setup needs re-implementing (reminder)\n";

                    // install the cull callback that will select the appropriate
                    // state based on the position of the camera.
                    _biomeRegionSelector = new BiomeRegionSelector(
                        _surface->getBiomeRegions(),
                        _textureDefs,
                        stateset,
                        _splatTexUnit );

                    engine->addCullCallback( _biomeRegionSelector.get() );
                }
            }

            if ( _landCover.valid() )
            {
                //gogolandcover( engine );
            }
        }
    }
}


void
SplatTerrainEffect::onUninstall(TerrainEngineNode* engine)
{
    if ( engine )
    {
        if ( _noiseTexUnit >= 0 )
        {
            engine->getResources()->releaseTextureImageUnit( _noiseTexUnit );
            _noiseTexUnit = -1;
        }
    
        if ( _splatTexUnit >= 0 )
        {
            engine->getResources()->releaseTextureImageUnit( _splatTexUnit );
            _splatTexUnit = -1;
        }

        if ( _biomeRegionSelector.valid() )
        {
            engine->removeCullCallback( _biomeRegionSelector.get() );
            _biomeRegionSelector = 0L;
        }
    }
}

bool
SplatTerrainEffect::createSplattingTextures(const Coverage*        coverage,
                                            const Surface*         surface,
                                            SplatTextureDefVector& output) const
{
    int numValidTextures = 0;

    if ( coverage == 0L || surface == 0L )
        return false;

    const BiomeRegionVector& biomeRegions = surface->getBiomeRegions();

    OE_INFO << LC << "Creating splatting textures for " << biomeRegions.size() << " biome regions\n";

    // Create a texture def for each biome region.
    for(unsigned b = 0; b < biomeRegions.size(); ++b)
    {
        const BiomeRegion& biomeRegion = biomeRegions[b];

        // Create a texture array and lookup table for this region:
        SplatTextureDef def;

        if ( biomeRegion.getCatalog() )
        {
            if ( biomeRegion.getCatalog()->createSplatTextureDef(_dbo.get(), def) )
            {
                // install the sampling function.
                createSplattingSamplingFunction( coverage, def );
                numValidTextures++;
            }
            else
            {
                OE_WARN << LC << "Failed to create a texture for a catalog (" 
                    << biomeRegion.getCatalog()->name().get() << ")\n";
            }
        }
        else
        {
            OE_WARN << LC << "Biome Region \""
                << biomeRegion.name().get() << "\"" 
                << " has an empty catalog and will be ignored.\n";
        }

        // put it on the list either way, since the vector indicies of biomes
        // and texturedefs need to line up
        output.push_back( def );
    }

    return numValidTextures > 0;
}

#define IND "    "

bool
SplatTerrainEffect::createSplattingSamplingFunction(const Coverage*  coverage,
                                                    SplatTextureDef& textureDef) const
{
    if ( !coverage || !coverage->getLegend() )
    {
        OE_WARN << LC << "Sampling function: illegal state (no coverage or legend); \n";
        return false;
    }

    if ( !textureDef._texture.valid() )
    {
        OE_WARN << LC << "Internal: texture is not set; cannot create a sampling function\n";
        return false;
    }

    std::stringstream
        weightBuf,
        primaryBuf,
        detailBuf,
        brightnessBuf,
        contrastBuf,
        thresholdBuf,
        slopeBuf;

    unsigned
        primaryCount    = 0,
        detailCount     = 0,
        brightnessCount = 0,
        contrastCount   = 0,
        thresholdCount  = 0,
        slopeCount      = 0;

    const SplatCoverageLegend::Predicates& preds = coverage->getLegend()->getPredicates();
    for(SplatCoverageLegend::Predicates::const_iterator p = preds.begin(); p != preds.end(); ++p)
    {
        const CoverageValuePredicate* pred = p->get();

        if ( pred->_exactValue.isSet() )
        {
            // Look up by class name:
            const std::string& className = pred->_mappedClassName.get();
            const SplatLUT::const_iterator i = textureDef._splatLUT.find(className);
            if ( i != textureDef._splatLUT.end() )
            {
                // found it; loop over the range selectors:
                int selectorCount = 0;
                const SplatSelectorVector& selectors = i->second;

                OE_DEBUG << LC << "Class " << className << " has " << selectors.size() << " selectors.\n";

                for(SplatSelectorVector::const_iterator selector = selectors.begin();
                    selector != selectors.end();
                    ++selector)
                {
                    const std::string&    expression = selector->first;
                    const SplatRangeData& rangeData  = selector->second;

                    std::string val = pred->_exactValue.get();

                    weightBuf
                        << IND "float w" << val
                        << " = (1.0-clamp(abs(value-" << val << ".0),0.0,1.0));\n";

                    // Primary texture index:
                    if ( primaryCount == 0 )
                        primaryBuf << IND "primary += ";
                    else
                        primaryBuf << " + ";

                    // the "+1" is because "primary" starts out at -1.
                    primaryBuf << "w"<<val << "*" << (rangeData._textureIndex + 1) << ".0";
                    primaryCount++;

                    // Detail texture index:
                    if ( rangeData._detail.isSet() )
                    {
                        if ( detailCount == 0 )
                            detailBuf << IND "detail += ";
                        else
                            detailBuf << " + ";
                        // the "+1" is because "detail" starts out at -1.
                        detailBuf << "w"<<val << "*" << (rangeData._detail->_textureIndex + 1) << ".0";
                        detailCount++;

                        if ( rangeData._detail->_brightness.isSet() )
                        {
                            if ( brightnessCount == 0 )
                                brightnessBuf << IND "brightness += ";
                            else
                                brightnessBuf << " + ";
                            brightnessBuf << "w"<<val << "*" << rangeData._detail->_brightness.get();
                            brightnessCount++;
                        }

                        if ( rangeData._detail->_contrast.isSet() )
                        {
                            if ( contrastCount == 0 )
                                contrastBuf << IND "contrast += ";
                            else
                                contrastBuf << " + ";
                            contrastBuf << "w"<<val << "*" << rangeData._detail->_contrast.get();
                            contrastCount++;
                        }

                        if ( rangeData._detail->_threshold.isSet() )
                        {
                            if ( thresholdCount == 0 )
                                thresholdBuf << IND "threshold += ";
                            else
                                thresholdBuf << " + ";
                            thresholdBuf << "w"<<val << "*" << rangeData._detail->_threshold.get();
                            thresholdCount++;
                        }

                        if ( rangeData._detail->_slope.isSet() )
                        {
                            if ( slopeCount == 0 )
                                slopeBuf << IND "slope += ";
                            else
                                slopeBuf << " + ";
                            slopeBuf << "w"<<val << "*" << rangeData._detail->_slope.get();
                            slopeCount++;
                        }
                    }                    
                }
            }
        }
    }

    if ( primaryCount > 0 )
        primaryBuf << ";\n";

    if ( detailCount > 0 )
        detailBuf << ";\n";

    if ( brightnessCount > 0 )
        brightnessBuf << ";\n";

    if ( contrastCount > 0 )
        contrastBuf << ";\n";

    if ( thresholdCount > 0 )
        thresholdBuf << ";\n";

    if ( slopeCount > 0 )
        slopeBuf << ";\n";

    SplattingShaders splatting;
    std::string code = ShaderLoader::load(
        splatting.FragGetRenderInfo,
        splatting);

    std::string codeToInject = Stringify()
        << IND
        << weightBuf.str()
        << primaryBuf.str()
        << detailBuf.str()
        << brightnessBuf.str()
        << contrastBuf.str()
        << thresholdBuf.str()
        << slopeBuf.str();

    osgEarth::replaceIn(code, "$COVERAGE_SAMPLING_FUNCTION", codeToInject);

    textureDef._samplingFunction = code;

    OE_DEBUG << LC << "Sampling function = \n" << code << "\n\n";

    return true;
}

osg::Texture*
SplatTerrainEffect::createNoiseTexture() const
{
    const int size = 1024;
    const int slices = 1;

    GLenum type = slices > 2 ? GL_RGBA : GL_LUMINANCE;
    
    osg::Image* image = new osg::Image();
    image->allocateImage(size, size, 1, type, GL_UNSIGNED_BYTE);

    // 0 = rocky mountains..
    // 1 = warping...
    const float F[4] = { 4.0f, 16.0f, 4.0f, 8.0f };
    const float P[4] = { 0.8f,  0.6f, 0.8f, 0.9f };
    const float L[4] = { 2.2f,  1.7f, 3.0f, 4.0f };
    
    for(int k=0; k<slices; ++k)
    {
        // Configure the noise function:
        osgEarth::Util::SimplexNoise noise;
        noise.setNormalize( true );
        noise.setRange( 0.0, 1.0 );
        noise.setFrequency( F[k] );
        noise.setPersistence( P[k] );
        noise.setLacunarity( L[k] );
        noise.setOctaves( 8 );

        float nmin = 10.0f;
        float nmax = -10.0f;

        // write repeating noise to the image:
        ImageUtils::PixelReader read ( image );
        ImageUtils::PixelWriter write( image );
        for(int t=0; t<size; ++t)
        {
            double rt = (double)t/size;
            for(int s=0; s<size; ++s)
            {
                double rs = (double)s/(double)size;

                double n = noise.getTiledValue(rs, rt);

                n = osg::clampBetween(n, 0.0, 1.0);

                if ( n < nmin ) nmin = n;
                if ( n > nmax ) nmax = n;
                osg::Vec4f v = read(s, t);
                v[k] = n;
                write(v, s, t);
            }
        }
   
        // histogram stretch to [0..1]
        for(int x=0; x<size*size; ++x)
        {
            int s = x%size, t = x/size;
            osg::Vec4f v = read(s, t);
            v[k] = osg::clampBetween((v[k]-nmin)/(nmax-nmin), 0.0f, 1.0f);
            write(v, s, t);
        }

        OE_INFO << LC << "Noise: MIN = " << nmin << "; MAX = " << nmax << "\n";
    }

#if 0
    std::string filename("noise.png");
    osgDB::writeImageFile(*image, filename);
    OE_NOTICE << LC << "Wrote noise texture to " << filename << "\n";
#endif

    // make a texture:
    osg::Texture2D* tex = new osg::Texture2D( image );
    tex->setWrap(tex->WRAP_S, tex->REPEAT);
    tex->setWrap(tex->WRAP_T, tex->REPEAT);
    tex->setFilter(tex->MIN_FILTER, tex->LINEAR_MIPMAP_LINEAR);
    tex->setFilter(tex->MAG_FILTER, tex->LINEAR);
    tex->setMaxAnisotropy( 1.0f );
    tex->setUnRefImageDataAfterApply( true );

    return tex;
}
