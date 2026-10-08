// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later

fn copy_history_framebuffer(fb: &mut GLFramebuffer, image: &GLImage) -> Result<()> {
    if image.size != fb.size || image.format != fb.format {
        Gl3Framebuffer::init(fb, image.size, image.format)?;
    }

    unsafe {
        // Keep the source at attachment zero while copying directly into the
        // history texture. No additional draw attachment or draw-buffer state
        // is needed, including on GLES.
        fb.ctx.bind_framebuffer(glow::READ_FRAMEBUFFER, Some(fb.fbo));
        fb.ctx.framebuffer_texture_2d(
            glow::READ_FRAMEBUFFER, glow::COLOR_ATTACHMENT0, glow::TEXTURE_2D, image.handle, 0,
        );
        fb.ctx.read_buffer(glow::COLOR_ATTACHMENT0);
        fb.ctx.bind_texture(glow::TEXTURE_2D, fb.image);
        fb.ctx.copy_tex_sub_image_2d(
            glow::TEXTURE_2D, 0, 0, 0, 0, 0, fb.size.width as i32, fb.size.height as i32,
        );
        fb.ctx.framebuffer_texture_2d(
            glow::READ_FRAMEBUFFER, glow::COLOR_ATTACHMENT0, glow::TEXTURE_2D, fb.image, 0,
        );
        fb.ctx.bind_framebuffer(glow::READ_FRAMEBUFFER, None);
    }
    Ok(())
}
