/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2009 Pelican Ventures, Inc.
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "MapService.h"
#include <osgEarth/TileSource>
#include <osgEarth/Registry>
#include <osg/Notify>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>

#include <sstream>
#include <iomanip>
#include <algorithm>

using namespace osgEarth;

#define PROPERTY_URL        "url"
#define PROPERTY_PROFILE    "profile"

class ArcGISSource : public TileSource
{
public:
    ArcGISSource( const osgDB::ReaderWriter::Options* options ) :
      TileSource( options )
    {
        if ( options )
        {
            // this is the ArcGIS REST services URL for the map service,
            // e.g. http://server/ArcGIS/rest/services/Layer/MapServer
            if ( options->getPluginData( PROPERTY_URL ) )
                _url = std::string( (const char*)options->getPluginData( PROPERTY_URL ) );

            // force a profile type
            if ( options->getPluginData( PROPERTY_PROFILE ) )
                _profile_str = std::string( (const char*)options->getPluginData( PROPERTY_PROFILE ) );
        }

        //TODO: allow single layers vs. "fused view"
        if ( _layer.empty() )
            _layer = "_alllayers"; // default to the AGS "fused view"

        //TODO: detect the format
        if ( _format.empty() )
            _format = "png";

        // read metadata from the server
        if ( !_map_service.init( _url ) )
        {
            osg::notify(osg::WARN) << "[osgearth] [ArcGIS] map service initialization failed: "
                << _map_service.getError() << std::endl;
        }
    }

    // override
    void initialize( const std::string& referenceURI, const Profile* overrideProfile)
    {
        const Profile* profile = NULL;

        if ( !_profile_str.empty() )
        {
            profile = Profile::create( _profile_str );
        }
        else if ( _map_service.getProfile() )
        {
            profile = _map_service.getProfile();

            if ( !_map_service.isTiled() )
            {
                // expand the profile's extents so they form a square.
                // AGS will return an image of a different extent than requested if the pixel aspect
                // ratio is not the same at the geoextent aspect ratio. By forcing a square full extent,
                // we can always request square tiles.

                const GeoExtent& oldEx = profile->getExtent();
                if ( oldEx.width() > oldEx.height() )
                {
                    double d = oldEx.width() - oldEx.height();
                    unsigned int tilesX, tilesY;
                    profile->getNumTiles( 0, tilesX, tilesY );
                    profile = Profile::create( profile->getSRS(), oldEx.xMin(), oldEx.yMin()-d/2, oldEx.xMax(), oldEx.yMax()+d/2, tilesX, tilesY );                    
                }
                else if ( oldEx.width() < oldEx.height() )
                {
                    double d = oldEx.height() - oldEx.width();
                    unsigned int tilesX, tilesY;
                    profile->getNumTiles( 0, tilesX, tilesY );
                    profile = Profile::create( profile->getSRS(), oldEx.xMin()-d/2, oldEx.yMin(), oldEx.xMax()+d/2, oldEx.yMax(), tilesX, tilesY );    
                }
            }
        }        
        else
        {
            profile = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
        }

		//Set the profile
		setProfile( profile );
    }

    // override
    int getPixelsPerTile() const
    {
      return _map_service.getTileInfo().getTileSize();
    }

    // override
    osg::Image* createImage( const TileKey* key,
                             ProgressCallback* progress)
    {
        std::stringstream buf;

        int level = key->getLevelOfDetail();

        unsigned int tile_x, tile_y;
        key->getTileXY( tile_x, tile_y );

        std::string f = _map_service.getTileInfo().getFormat();
        std::transform( f.begin(), f.end(), f.begin(), tolower );
        if ( f.length() > 3 && f.substr( 0, 3 ) == "png" )
            f = "png";

        if ( _map_service.isTiled() )
        {
            buf << _url << "/tile"
                << "/" << level
                << "/" << tile_y
                << "/" << tile_x << "." << f;
        }
        else
        {
            const GeoExtent& ex = key->getGeoExtent();

            buf << std::setprecision(16)
                << _url << "/export"
                << "?bbox=" << ex.xMin() << "," << ex.yMin() << "," << ex.xMax() << "," << ex.yMax()
                << "&format=" << f 
                << "&size=256,256"
                << "&transparent=true"
                << "&f=image"
                << "&" << "." << f;
        }

        //osg::notify(osg::NOTICE) << "Key = " << key->str() << ", URL = " << buf.str() << std::endl;
        //return osgDB::readImageFile( buf.str(), getOptions() );
        return HTTPClient::readImageFile( buf.str(), getOptions(), progress );
    }

    // override
    osg::HeightField* createHeightField( const TileKey* key,
                                         ProgressCallback* progress)
    {
        //TODO
        return NULL;
    }

    // override
    virtual std::string getExtension() const 
    {
        return _format;
    }

private:
    //osg::ref_ptr<const osgDB::ReaderWriter::Options> _options;
    std::string _url;
    std::string _profile_str;
    std::string _map;
    std::string _layer;
    std::string _format;
    MapService _map_service;
    //osg::ref_ptr<const Profile> manual_profile;
};


class ReaderWriterArcGIS : public osgDB::ReaderWriter
{
public:
    ReaderWriterArcGIS()
    {
        supportsExtension( "osgearth_arcgis", "ArcGIS Server" );
    }

    virtual const char* className()
    {
        return "ArcGIS Server REST ReaderWriter";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return new ArcGISSource(options);
    }
};

REGISTER_OSGPLUGIN(osgearth_arcgis, ReaderWriterArcGIS);

