#![allow (clippy::excessive_precision)]

pub fn Submit () -> u32
{
// crate::Submit_System (1, 2, "Solar System", 9, 0.0, 0.0, 0_i64, 0_i64, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0x00888888);

   crate::Submit_Body (2, 1245, "Sun", 10, 695700000.0, 1.988410e+30, 0x00FFDD66, 0.05881349945180604, 0.9898366082510357, 0.023246492014369433, -0.12737370944329088, 0.0, 0.0, 0.0);

   crate::Submit_Surface (1245, 1246, "Sun Surface", "sun.jpg", 1.4691483511587469, 140341220_i64);

   2
}
