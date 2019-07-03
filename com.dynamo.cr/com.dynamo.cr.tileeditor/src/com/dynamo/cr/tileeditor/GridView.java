package com.dynamo.cr.tileeditor;

import java.awt.image.BufferedImage;
import java.util.List;
import java.util.Map;

import javax.inject.Inject;
import javax.vecmath.Point2f;

import org.eclipse.swt.graphics.Rectangle;

import com.dynamo.cr.tileeditor.core.IGridView;
import com.dynamo.cr.tileeditor.core.Layer;
import com.dynamo.cr.tileeditor.core.Layer.Cell;
import com.dynamo.cr.tileeditor.core.MapBrush;

public class GridView implements IGridView {

    @Inject private GridRenderer renderer;
    @Inject private IGridEditorOutlinePage outline;
    @Inject private GridEditor gridEditor;

    @Override
    public void setTileSet(BufferedImage image, int tileWidth, int tileHeight,
            int tileMargin, int tileSpacing) {
        this.renderer.setTileSet(image, tileWidth, tileHeight, tileMargin, tileSpacing);
    }

    @Override
    public void setLayers(List<Layer> layers) {
        this.renderer.setLayers(layers);
        this.outline.setInput(layers);
    }

    @Override
    public void setSelectedLayer(Layer layer) {
        this.renderer.setSelectedLayer(layer);
        this.outline.setSelectedLayer(layer);
    }

    @Override
    public void setCells(int layerIndex, Map<Long, Cell> cells) {
        this.renderer.setCells(layerIndex, cells);
    }

    @Override
    public void setCell(int layerIndex, long cellIndex, Cell cell) {
        this.renderer.setCell(layerIndex, cellIndex, cell);
    }

    @Override
    public void refreshProperties() {
        this.outline.refresh();
        this.gridEditor.refreshProperties();
    }

    @Override
    public void setValidModel(boolean valid) {
        this.renderer.setEnabled(valid);
    }

    @Override
    public void setDirty(boolean dirty) {
        gridEditor.fireDirty();
    }

    @Override
    public Rectangle getPreviewRect() {
        return this.renderer.getViewRect();
    }

    @Override
    public void setPreview(Point2f position, float zoom) {
        this.renderer.setCamera(position, zoom);
    }

    @Override
    public void setBrush(MapBrush brush) {
        this.renderer.setBrush(brush);
    }

}